// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/renderer/translate/page_translator_bindings.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace compass::translate {

namespace {

constexpr char kObjectName[] = "compassTranslate";
// Far more than a batch the page translator sends (16 segments, about
// 6,000 characters): a page's script can't reach this world, but keep the
// browser's work bounded anyway.
constexpr uint32_t kMaxSegments = 64;
constexpr size_t kMaxCharacters = 64 * 1024;

const char* ResultName(mojom::PageTranslationResult result) {
  switch (result) {
    case mojom::PageTranslationResult::kOk:
      return "ok";
    case mojom::PageTranslationResult::kUnsupportedLanguage:
      return "unsupported";
    case mojom::PageTranslationResult::kDeclined:
      return "declined";
    case mojom::PageTranslationResult::kFailed:
      return "failed";
  }
  return "failed";
}

}  // namespace

// static
void PageTranslatorBindings::Create(content::RenderFrame* render_frame) {
  new PageTranslatorBindings(render_frame);  // Deletes itself (OnDestruct).
}

PageTranslatorBindings::PageTranslatorBindings(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

PageTranslatorBindings::~PageTranslatorBindings() = default;

void PageTranslatorBindings::OnDestruct() {
  delete this;
}

void PageTranslatorBindings::DidCreateScriptContext(
    v8::Local<v8::Context> context,
    int32_t world_id) {
  if (world_id != ISOLATED_WORLD_ID_TRANSLATE) {
    return;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  context_.Reset(isolate, context);
  v8::Local<v8::Object> object = v8::Object::New(isolate);
  v8::Local<v8::Function> send =
      gin::CreateFunctionTemplate(
          isolate, base::BindRepeating(&PageTranslatorBindings::Send,
                                       weak_factory_.GetWeakPtr()))
          ->GetFunction(context)
          .ToLocalChecked();
  object->Set(context, gin::StringToV8(isolate, "send"), send).Check();
  context->Global()
      ->Set(context, gin::StringToV8(isolate, kObjectName), object)
      .Check();
}

void PageTranslatorBindings::WillReleaseScriptContext(
    v8::Local<v8::Context> context,
    int32_t world_id) {
  if (world_id == ISOLATED_WORLD_ID_TRANSLATE) {
    context_.Reset();
  }
}

void PageTranslatorBindings::Send(gin::Arguments* args) {
  std::string source;
  std::string target;
  v8::Local<v8::Value> list_value;
  v8::Local<v8::Function> callback;
  if (!args->GetNext(&source) || !args->GetNext(&target) ||
      !args->GetNext(&list_value) || !list_value->IsArray() ||
      !args->GetNext(&callback)) {
    args->ThrowTypeError("send(source, target, segments, callback)");
    return;
  }
  v8::Local<v8::Array> list = list_value.As<v8::Array>();
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (list->Length() > kMaxSegments) {
    args->ThrowTypeError("Too many segments");
    return;
  }
  std::vector<mojom::TranslationSegmentPtr> segments;
  size_t characters = 0;
  for (uint32_t i = 0; i < list->Length(); ++i) {
    v8::Local<v8::Value> item;
    if (!list->Get(context, i).ToLocal(&item) || !item->IsObject()) {
      args->ThrowTypeError("A segment is {text, html}");
      return;
    }
    v8::Local<v8::Object> object = item.As<v8::Object>();
    v8::Local<v8::Value> text_value;
    v8::Local<v8::Value> html_value;
    std::string text;
    if (!object->Get(context, gin::StringToV8(isolate, "text"))
             .ToLocal(&text_value) ||
        !gin::ConvertFromV8(isolate, text_value, &text) ||
        !object->Get(context, gin::StringToV8(isolate, "html"))
             .ToLocal(&html_value)) {
      args->ThrowTypeError("A segment is {text, html}");
      return;
    }
    characters += text.size();
    segments.push_back(
        mojom::TranslationSegment::New(std::move(text), html_value->IsTrue()));
  }
  if (characters > kMaxCharacters) {
    args->ThrowTypeError("Too much text");
    return;
  }
  translator()->Translate(
      source, target, std::move(segments),
      base::BindOnce(&PageTranslatorBindings::OnTranslated,
                     weak_factory_.GetWeakPtr(),
                     v8::Global<v8::Function>(isolate, callback)));
}

void PageTranslatorBindings::OnTranslated(
    v8::Global<v8::Function> callback,
    mojom::PageTranslationResult result,
    const std::vector<std::optional<std::string>>& translations) {
  if (context_.IsEmpty()) {
    return;  // The world went away (another page).
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = context_.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Array> list = v8::Array::New(isolate, translations.size());
  {
    // Building the answer runs no script; the call below runs the callback
    // and its microtasks, as Blink does.
    v8::MicrotasksScope microtasks(context,
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    for (size_t i = 0; i < translations.size(); ++i) {
      v8::Local<v8::Value> value =
          translations[i]
              ? v8::Local<v8::Value>(gin::StringToV8(isolate, *translations[i]))
              : v8::Local<v8::Value>(v8::Null(isolate));
      list->CreateDataProperty(context, static_cast<uint32_t>(i), value)
          .Check();
    }
  }
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate, ResultName(result)),
                                 list};
  render_frame()->GetWebFrame()->CallFunctionEvenIfScriptDisabled(
      callback.Get(isolate), v8::Undefined(isolate), std::size(argv), argv);
}

mojom::PageTranslator* PageTranslatorBindings::translator() {
  if (!translator_.is_bound()) {
    render_frame()->GetBrowserInterfaceBroker().GetInterface(
        translator_.BindNewPipeAndPassReceiver());
  }
  return translator_.get();
}

}  // namespace compass::translate
