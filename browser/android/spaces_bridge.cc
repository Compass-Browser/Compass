// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/spaces_bridge.h"

#include <map>
#include <utility>

#include "base/android/callback_android.h"
#include "base/android/scoped_java_ref.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/live_folders/live_folder_store.h"
#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "compass/browser/live_folders/live_plugin_package.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "url/gurl.h"

// Must come after the class declaration.
#include "compass/android/jni_headers/SpacesBridge_jni.h"

namespace compass {

namespace {

using base::android::JavaParamRef;
using base::android::ScopedJavaGlobalRef;

constexpr char kNoLiveFolders[] = "Live folders aren't available here.";

// What the editor needs of a plugin (LiveFoldersState.kt parses it): as the
// desktop's sidebar gets it (sidebar_page_handler.cc, ToMojoPlugin()).
base::DictValue PluginJson(const LivePlugin& plugin,
                           const LiveFolderStore& store) {
  const LivePluginManifest& manifest = plugin.manifest;
  base::ListValue config;
  using Type = LivePluginManifest::ConfigField::Type;
  for (const auto& field : manifest.config) {
    base::DictValue f;
    f.Set("key", field.key);
    switch (field.type) {
      case Type::kString:
        f.Set("type", "string");
        break;
      case Type::kUrl:
        f.Set("type", "url");
        break;
      case Type::kNumber:
        f.Set("type", "number");
        break;
      case Type::kBool:
        f.Set("type", "bool");
        break;
      case Type::kSelect:
        f.Set("type", "select");
        break;
    }
    f.Set("label", field.label);
    f.Set("placeholder", field.placeholder);
    f.Set("optional", field.optional);
    if (field.default_value) {
      f.Set("default", field.default_value->Clone());
    }
    if (field.min) {
      f.Set("min", *field.min);
    }
    if (field.max) {
      f.Set("max", *field.max);
    }
    base::ListValue options;
    for (const auto& option : field.options) {
      options.Append(base::DictValue()
                         .Set("value", option.value)
                         .Set("label", option.label));
    }
    f.Set("options", std::move(options));
    config.Append(std::move(f));
  }
  base::ListValue secrets;
  for (const auto& secret : manifest.secrets) {
    secrets.Append(base::DictValue()
                       .Set("key", secret.key)
                       .Set("label", secret.label)
                       .Set("help", secret.help)
                       .Set("optional", secret.optional)
                       .Set("set", store.HasSecret(manifest.id, secret.key)));
  }
  base::ListValue cookie_hosts;
  for (const std::string& host : manifest.cookies) {
    cookie_hosts.Append(host);
  }
  base::ListValue hosts;
  for (const std::string& host : manifest.hosts) {
    hosts.Append(host);
  }
  return base::DictValue()
      .Set("id", manifest.id)
      .Set("name", manifest.name)
      .Set("description", manifest.description)
      .Set("builtin", plugin.builtin)
      .Set("local", plugin.local)
      .Set("config", std::move(config))
      .Set("secrets", std::move(secrets))
      .Set("hosts", std::move(hosts))
      .Set("cookieHosts", std::move(cookie_hosts));
}

base::DictValue PreviewJson(const LivePluginPackage& package) {
  const LivePluginManifest& manifest = package.manifest;
  base::ListValue hosts;
  for (const std::string& host : manifest.hosts) {
    hosts.Append(host);
  }
  base::ListValue cookie_hosts;
  for (const std::string& host : manifest.cookies) {
    cookie_hosts.Append(host);
  }
  base::ListValue secrets;
  for (const auto& secret : manifest.secrets) {
    secrets.Append(secret.label);
  }
  return base::DictValue()
      .Set("id", manifest.id)
      .Set("name", manifest.name)
      .Set("description", manifest.description)
      .Set("version", manifest.version)
      .Set("hosts", std::move(hosts))
      .Set("cookieHosts", std::move(cookie_hosts))
      .Set("secrets", std::move(secrets))
      .Set("source", package.source.spec())
      .Set("sha256", package.sha256);
}

std::string ToJson(const base::DictValue& dict) {
  return base::WriteJson(dict).value_or("{}");
}

}  // namespace

SpacesBridge::SpacesBridge(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& java_object,
    Profile* profile)
    : java_object_(env, java_object), spaces_(SpacesAndroid::Get(profile)) {
  observation_.Observe(spaces_.get());
}

SpacesBridge::~SpacesBridge() = default;

void SpacesBridge::Destroy(JNIEnv* env) {
  delete this;
}

std::string SpacesBridge::Snapshot(JNIEnv* env) {
  if (spaces_->loaded()) {
    // Adopts the list's active space until this window picks one, and again
    // once its space is gone (removed here, or by sync).
    space_id_ = spaces_->SpaceOrActive(space_id_).id();
  }
  return spaces_->Snapshot(space_id_);
}

std::string SpacesBridge::ShownSpace(JNIEnv* env) {
  return spaces_->loaded() ? spaces_->SpaceOrActive(space_id_)
                                 .id()
                                 .AsLowercaseString()
                           : std::string();
}

void SpacesBridge::ShowSpace(JNIEnv* env, const std::string& space_id) {
  base::Uuid id = base::Uuid::ParseLowercase(space_id);
  if (id.is_valid()) {
    space_id_ = id;
  }
}

void SpacesBridge::SetActiveSpace(JNIEnv* env, const std::string& space_id) {
  if (SpaceModel* space =
          spaces_->list().FindSpace(base::Uuid::ParseLowercase(space_id))) {
    space_id_ = space->id();
    // The list's active space is the last one a window showed.
    spaces_->list().SetActiveSpace(*space);
  }
}

std::string SpacesBridge::AddSpace(JNIEnv* env, const std::u16string& name) {
  SpaceList& list = spaces_->list();
  SpaceModel* space = list.AddSpace(list.spaces().size(), name);
  if (!space) {
    return std::string();
  }
  space_id_ = space->id();
  list.SetActiveSpace(*space);
  return space->id().AsLowercaseString();
}

void SpacesBridge::EditSpace(JNIEnv* env,
                             const std::string& space_id,
                             const std::u16string& name,
                             const std::string& emoji,
                             const std::string& tint) {
  SpaceModel* space =
      spaces_->list().FindSpace(base::Uuid::ParseLowercase(space_id));
  if (!space) {
    return;
  }
  if (!name.empty() && name != space->name()) {
    space->SetName(name);
  }
  std::optional<SpaceIcon> icon =
      emoji.empty() ? SpaceIcon() : SpaceIcon::Emoji(emoji);
  if (icon && *icon != space->icon()) {
    space->SetIcon(*icon);
  }
  SpaceColor color;
  uint32_t rgb = 0;
  if (tint.size() == 7 && tint[0] == '#' &&
      base::HexStringToUInt(std::string_view(tint).substr(1), &rgb)) {
    color = SpaceColor::FromRGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff,
                                rgb & 0xff);
  }
  if (color != space->color()) {
    space->SetColor(color);
  }
}

void SpacesBridge::RemoveSpace(JNIEnv* env, const std::string& space_id) {
  SpaceList& list = spaces_->list();
  SpaceModel* space = list.FindSpace(base::Uuid::ParseLowercase(space_id));
  if (space && list.spaces().size() > 1) {
    list.RemoveSpace(*space);
  }
}

void SpacesBridge::Pin(JNIEnv* env, int32_t tab_id, bool favorite) {
  spaces_->Pin(tab_id, favorite);
}

void SpacesBridge::MoveNode(JNIEnv* env,
                            const std::string& node_id,
                            const std::string& space_id,
                            const std::string& section) {
  SpacesAndroid::Section where = section == "favorites"
                                     ? SpacesAndroid::Section::kFavorites
                                 : section == "today"
                                     ? SpacesAndroid::Section::kToday
                                     : SpacesAndroid::Section::kPinned;
  // Within this window's space unless another is named.
  base::Uuid space = space_id.empty() ? spaces_->SpaceOrActive(space_id_).id()
                                      : base::Uuid::ParseLowercase(space_id);
  spaces_->MoveNode(base::Uuid::ParseLowercase(node_id), space, where);
}

void SpacesBridge::RemoveNode(JNIEnv* env, const std::string& node_id) {
  SidebarNode* node = spaces_->FindNode(base::Uuid::ParseLowercase(node_id));
  if (!node) {
    return;
  }
  SpaceList& list = spaces_->list();
  SpaceModel* space = list.favorites().Contains(node) ? &list.favorites()
                                                       : list.FindSpaceOf(node);
  if (space) {
    space->Remove(node);
  }
}

void SpacesBridge::SetCollapsed(JNIEnv* env,
                                const std::string& folder_id,
                                bool collapsed) {
  SidebarNode* node =
      spaces_->FindNode(base::Uuid::ParseLowercase(folder_id));
  if (!node || node->type() != SidebarNode::Type::kFolder) {
    return;
  }
  if (SpaceModel* space = spaces_->list().FindSpaceOf(node)) {
    space->SetCollapsed(node, collapsed);
  }
}

int32_t SpacesBridge::TabFor(JNIEnv* env, const std::string& node_id) {
  return spaces_->TabFor(base::Uuid::ParseLowercase(node_id));
}

void SpacesBridge::TabOpened(JNIEnv* env,
                             int32_t tab_id,
                             const std::string& url,
                             const std::u16string& title,
                             bool restored) {
  spaces_->TabOpened(tab_id, GURL(url), title, restored, space_id_);
}

void SpacesBridge::TabOpenedForNode(JNIEnv* env,
                                    int32_t tab_id,
                                    const std::string& node_id) {
  spaces_->TabOpenedForNode(tab_id, base::Uuid::ParseLowercase(node_id));
}

void SpacesBridge::TabUpdated(JNIEnv* env,
                              int32_t tab_id,
                              const std::string& url,
                              const std::u16string& title) {
  spaces_->TabUpdated(tab_id, GURL(url), title);
}

void SpacesBridge::TabSelected(JNIEnv* env, int32_t tab_id) {
  spaces_->TabSelected(tab_id);
}

void SpacesBridge::TabClosed(JNIEnv* env, int32_t tab_id) {
  spaces_->TabClosed(tab_id);
}

void SpacesBridge::LivePlugins(JNIEnv* env,
                               const JavaParamRef<jobject>& callback) {
  ScopedJavaGlobalRef<jobject> java_callback(env, callback);
  LiveFolderService* service = spaces_->live_folders();
  if (!service) {
    base::android::RunStringCallbackAndroid(java_callback, "[]");
    return;
  }
  // Installed plugins may have changed on disk: read them again each time,
  // as the desktop's editor does.
  service->RescanPlugins(base::BindOnce(
      [](base::WeakPtr<SpacesBridge> bridge,
         ScopedJavaGlobalRef<jobject> callback) {
        base::ListValue plugins;
        LiveFolderService* service =
            bridge ? bridge->spaces_->live_folders() : nullptr;
        if (service) {
          for (const LivePlugin& plugin : service->registry().plugins()) {
            plugins.Append(PluginJson(plugin, service->store()));
          }
        }
        base::android::RunStringCallbackAndroid(
            callback, base::WriteJson(plugins).value_or("[]"));
      },
      weak_factory_.GetWeakPtr(), std::move(java_callback)));
}

std::string SpacesBridge::LiveFolderSource(JNIEnv* env,
                                           const std::string& folder_id) {
  SidebarNode* node = spaces_->FindNode(base::Uuid::ParseLowercase(folder_id));
  LiveFolderService* service = spaces_->live_folders();
  if (!node || !node->is_live_folder() || !service) {
    return std::string();
  }
  const LiveSource& source = *node->live_source();
  return ToJson(
      base::DictValue()
          .Set("pluginId", source.plugin_id)
          .Set("title", base::UTF16ToUTF8(node->title()))
          .Set("config", source.config.Clone())
          .Set("cookiesGranted", service->store().CookiesGranted(node->id()))
          .Set("pluginUrl", source.plugin_url.is_valid()
                                ? source.plugin_url.spec()
                                : std::string())
          .Set("pluginSha256", source.plugin_sha256));
}

std::string SpacesBridge::SaveLiveFolder(JNIEnv* env,
                                         const std::string& folder_id,
                                         const std::string& plugin_id,
                                         const std::u16string& title,
                                         const std::string& config_json,
                                         const std::string& secrets_json,
                                         bool grant_cookies) {
  LiveFolderService* service = spaces_->live_folders();
  if (!service || !spaces_->loaded()) {
    return kNoLiveFolders;
  }
  std::optional<base::Value> config =
      base::JSONReader::Read(config_json, base::JSON_PARSE_RFC);
  std::optional<base::Value> secrets_value =
      base::JSONReader::Read(secrets_json, base::JSON_PARSE_RFC);
  if (!config || !config->is_dict() || !secrets_value ||
      !secrets_value->is_dict()) {
    return "The settings couldn't be read.";
  }
  std::map<std::string, std::string> secrets;
  for (const auto& [key, value] : secrets_value->GetDict()) {
    if (value.is_string()) {
      secrets[key] = value.GetString();
    }
  }
  if (!folder_id.empty()) {
    SidebarNode* folder =
        spaces_->FindNode(base::Uuid::ParseLowercase(folder_id));
    auto result = service->EditLiveFolder(folder, std::move(config->GetDict()),
                                          secrets, grant_cookies);
    return result.has_value() ? std::string() : result.error();
  }
  SpaceModel& space = spaces_->SpaceOrActive(space_id_);
  SidebarNode* parent = space.pinned_root();
  auto result = service->CreateLiveFolder(
      space, parent, parent->children().size(), title, plugin_id,
      std::move(config->GetDict()), secrets, grant_cookies);
  return result.has_value() ? std::string() : result.error();
}

void SpacesBridge::RefreshLiveFolder(JNIEnv* env,
                                     const std::string& folder_id) {
  if (LiveFolderService* service = spaces_->live_folders()) {
    service->Refresh(base::Uuid::ParseLowercase(folder_id));
  }
}

void SpacesBridge::MarkAllSeen(JNIEnv* env, const std::string& folder_id) {
  SidebarNode* node = spaces_->FindNode(base::Uuid::ParseLowercase(folder_id));
  SpaceModel* space = node ? spaces_->list().FindSpaceOf(node) : nullptr;
  if (space && node->is_live_folder()) {
    space->MarkAllSeen(node);
  }
}

void SpacesBridge::ConvertLiveFolder(JNIEnv* env,
                                     const std::string& folder_id) {
  SidebarNode* node = spaces_->FindNode(base::Uuid::ParseLowercase(folder_id));
  SpaceModel* space = node ? spaces_->list().FindSpaceOf(node) : nullptr;
  if (space && node->is_live_folder()) {
    space->ConvertToPlainFolder(node);
  }
}

void SpacesBridge::PreviewLivePlugin(JNIEnv* env,
                                     const std::string& url,
                                     const JavaParamRef<jobject>& callback) {
  ScopedJavaGlobalRef<jobject> java_callback(env, callback);
  LiveFolderService* service = spaces_->live_folders();
  if (!service) {
    base::android::RunStringCallbackAndroid(
        java_callback, ToJson(base::DictValue().Set("error", kNoLiveFolders)));
    return;
  }
  service->PreviewPlugin(
      GURL(url),
      base::BindOnce(
          [](ScopedJavaGlobalRef<jobject> callback,
             base::expected<LivePluginPackage, std::string> package) {
            base::android::RunStringCallbackAndroid(
                callback,
                ToJson(package.has_value()
                           ? base::DictValue().Set("preview",
                                                   PreviewJson(*package))
                           : base::DictValue().Set("error", package.error())));
          },
          std::move(java_callback)));
}

void SpacesBridge::InstallLivePlugin(JNIEnv* env,
                                     const std::string& url,
                                     const std::string& sha256,
                                     const JavaParamRef<jobject>& callback) {
  ScopedJavaGlobalRef<jobject> java_callback(env, callback);
  LiveFolderService* service = spaces_->live_folders();
  if (!service) {
    base::android::RunStringCallbackAndroid(java_callback, kNoLiveFolders);
    return;
  }
  service->InstallPlugin(
      GURL(url), sha256,
      base::BindOnce(
          [](ScopedJavaGlobalRef<jobject> callback, const std::string& error) {
            base::android::RunStringCallbackAndroid(callback, error);
          },
          std::move(java_callback)));
}

void SpacesBridge::RemoveLivePlugin(JNIEnv* env,
                                    const std::string& plugin_id,
                                    const JavaParamRef<jobject>& callback) {
  ScopedJavaGlobalRef<jobject> java_callback(env, callback);
  LiveFolderService* service = spaces_->live_folders();
  if (!service) {
    base::android::RunStringCallbackAndroid(java_callback, std::string());
    return;
  }
  service->RemovePlugin(
      plugin_id, base::BindOnce(
                     [](ScopedJavaGlobalRef<jobject> callback) {
                       base::android::RunStringCallbackAndroid(callback,
                                                               std::string());
                     },
                     std::move(java_callback)));
}

void SpacesBridge::OnSpacesChanged() {
  Java_SpacesBridge_onChanged(jni_zero::AttachCurrentThread(),
                              java_object_);
}

static int64_t JNI_SpacesBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(new SpacesBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(SpacesBridge)
