// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_

#include <jni.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "compass/browser/claude/page_reader.h"
#include "compass/browser/claude/sse_parser.h"

class Profile;

namespace content {
class WebContents;
}

namespace compass {

namespace claude {
class ClaudeClient;
}

// The native half of it.cipi1965.compass.claude.ClaudeBridge: a chat with
// Claude about the page on screen (docs/design/android.md, C8), on the
// desktop's client (ClaudeClient), key storage (ClaudeCredentials) and page
// reader. Each question carries the page's text.
class ClaudeBridge {
 public:
  ClaudeBridge(JNIEnv* env,
               const base::android::JavaRef<jobject>& java_object,
               Profile* profile);
  ClaudeBridge(const ClaudeBridge&) = delete;
  ClaudeBridge& operator=(const ClaudeBridge&) = delete;
  ~ClaudeBridge();

  void Destroy(JNIEnv* env);
  // The conversation, whether a reply streams, the last error and whether
  // there's a key, as JSON.
  std::string Snapshot(JNIEnv* env);
  void SetApiKey(JNIEnv* env, const std::string& key);
  // Asks |text| about the page |contents| shows (null: no page).
  void Send(JNIEnv* env,
            const std::string& text,
            content::WebContents* contents);
  void Stop(JNIEnv* env);
  void Clear(JNIEnv* env);

 private:
  struct Message {
    std::string role;
    std::string text;
  };

  void OnPage(std::string question,
              base::expected<claude::PageText, std::string> page);
  void OnKey(std::string content, std::optional<std::string> key);
  void OnEvent(const claude::SseParser::Event& event);
  void OnDone(std::optional<std::string> error);
  void Changed();

  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<Profile> profile_;
  std::vector<Message> messages_;
  std::unique_ptr<claude::ClaudeClient> client_;
  bool streaming_ = false;
  std::string error_;
  base::WeakPtrFactory<ClaudeBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_
