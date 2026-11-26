// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ReaderBridge.java's natives (docs/design/android.md, "Reader mode"): the
// desktop's reader scripts and reader page (docs/design/reader-mode.md),
// which android_web_ui_configs.cc registers.

#include <string>
#include <utility>

#include "base/android/callback_android.h"
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/reader/reader_assets.h"
#include "compass/browser/ui/webui/reader/reader_untrusted_ui.h"
#include "compass/strings/grit/compass_strings.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/ReaderBridge_jni.h"
#include "ui/base/l10n/l10n_util.h"

using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;

namespace compass {

namespace {

// As the desktop's ReaderTabState: Readability-readerable.js and reader.js,
// then check() (small, on every page load)...
const std::u16string& GetCheckScript() {
  static const base::NoDestructor<std::u16string> script(
      base::UTF8ToUTF16(base::StrCat(
          {reader::assets::kReaderable, "\n;\n", reader::assets::kReaderJs,
           "\n;__compassReader.check({});"})));
  return *script;
}

// ...and with Readability, parse().
const std::u16string& GetParseScript() {
  static const base::NoDestructor<std::u16string> script(
      base::UTF8ToUTF16(base::StrCat(
          {reader::assets::kReadability, "\n;\n", reader::assets::kReaderable,
           "\n;\n", reader::assets::kReaderJs,
           "\n;__compassReader.parse({});"})));
  return *script;
}

// In an isolated world of the main frame: the page's scripts can't see or
// change it, and its CSP doesn't apply.
void RunInPage(content::WebContents* contents,
               const std::u16string& script,
               base::OnceCallback<void(base::Value)> callback) {
  content::RenderFrameHost* frame =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!frame || !frame->IsRenderFrameLive()) {
    std::move(callback).Run(base::Value());
    return;
  }
  frame->ExecuteJavaScriptInIsolatedWorld(script, std::move(callback),
                                          ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

}  // namespace

static void JNI_ReaderBridge_Check(JNIEnv* env,
                                   content::WebContents* contents,
                                   const JavaRef<jobject>& j_callback) {
  RunInPage(contents, GetCheckScript(),
            base::BindOnce(
                [](ScopedJavaGlobalRef<jobject> callback, base::Value result) {
                  const base::DictValue* dict = result.GetIfDict();
                  base::android::RunBooleanCallbackAndroid(
                      callback,
                      dict && dict->FindBool("readerable").value_or(false));
                },
                ScopedJavaGlobalRef<jobject>(j_callback)));
}

// Parses the page and keeps the article for the reader page, as the
// desktop's ReaderTabState::Show(): the callback gets the token, which
// pageUrl() makes the reader page's URL and release() drops. A page that
// couldn't be read gets the reader page's own "no article" message.
static void JNI_ReaderBridge_Parse(JNIEnv* env,
                                   content::WebContents* contents,
                                   const JavaRef<jobject>& j_callback) {
  if (!contents) {
    base::android::RunStringCallbackAndroid(
        ScopedJavaGlobalRef<jobject>(j_callback), std::string());
    return;
  }
  content::BrowserContext* context = contents->GetBrowserContext();
  RunInPage(
      contents, GetParseScript(),
      base::BindOnce(
          [](content::BrowserContext* context,
             ScopedJavaGlobalRef<jobject> callback, base::Value result) {
            if (!result.is_dict()) {
              result = base::Value(
                  base::DictValue()
                      .Set("ok", false)
                      .Set("error", l10n_util::GetStringUTF8(
                                        IDS_COMPASS_READER_PAGE_UNREADABLE)));
            }
            std::string json;
            base::JSONWriter::Write(result, &json);
            base::android::RunStringCallbackAndroid(
                callback, ReaderArticleStore::Get().Add(context, std::move(json)));
          },
          // The tab's profile outlives the page's isolated world call.
          context, ScopedJavaGlobalRef<jobject>(j_callback)));
}

static std::string JNI_ReaderBridge_PageUrl(JNIEnv* env,
                                            const std::string& token) {
  return GetReaderPageUrl(token).spec();
}

static void JNI_ReaderBridge_Release(JNIEnv* env, const std::string& token) {
  ReaderArticleStore::Get().Remove(token);
}

}  // namespace compass

DEFINE_JNI(ReaderBridge)
