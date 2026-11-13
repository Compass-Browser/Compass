// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/devtools_format.h"

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude::devtools_format {
namespace {

TEST(DevToolsFormatTest, SecretNames) {
  for (const char* name :
       {"Authorization", "cookie", "Set-Cookie", "x-csrf-token", "X-API-Key",
        "session_id", "password", "client_secret", "refresh_token"}) {
    EXPECT_TRUE(IsSecretName(name)) << name;
  }
  for (const char* name : {"content-type", "accept", "theme", "cart"}) {
    EXPECT_FALSE(IsSecretName(name)) << name;
  }
  EXPECT_TRUE(LooksSecret("Bearer abc"));
  EXPECT_TRUE(LooksSecret("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.sig"));
  EXPECT_FALSE(LooksSecret("dark"));
  EXPECT_EQ(Hidden("abcd"), "<hidden, 4 characters>");
}

TEST(DevToolsFormatTest, MasksFieldsInText) {
  EXPECT_EQ(MaskSecretsInText("user=ada&password=hunter2&x=1"),
            "user=ada&password=<hidden>&x=1");
  EXPECT_EQ(MaskSecretsInText("https://a.test/cb?code=1&access_token=xyz"),
            "https://a.test/cb?code=1&access_token=<hidden>");
  EXPECT_EQ(MaskSecretsInText(R"({"user":"ada","api_key":"k\"1","n":2})"),
            R"({"user":"ada","api_key":"<hidden>","n":2})");
  EXPECT_EQ(MaskSecretsInText("id eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.sig"),
            "id <hidden JWT>");
  EXPECT_EQ(MaskSecretsInText("nothing here"), "nothing here");
}

TEST(DevToolsFormatTest, Headers) {
  base::DictValue headers;
  headers.Set("Content-Type", "text/html");
  headers.Set("Authorization", "Bearer xyz");
  headers.Set("X-Thing", "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.sig");
  EXPECT_EQ(FormatHeaders(headers, false),
            "  authorization: <hidden, 10 characters>\n"
            "  content-type: text/html\n"
            "  x-thing: <hidden, 40 characters>\n");
  EXPECT_NE(FormatHeaders(headers, true).find("Bearer xyz"),
            std::string::npos);
}

TEST(DevToolsFormatTest, SizesAndTimes) {
  EXPECT_EQ(FormatBytes(512), "512 B");
  EXPECT_EQ(FormatBytes(3200), "3.2 kB");
  EXPECT_EQ(FormatBytes(1400000), "1.4 MB");
  EXPECT_EQ(FormatDuration(0.12), "120 ms");
  EXPECT_EQ(FormatDuration(1.44), "1.4 s");
}

TEST(DevToolsFormatTest, CutsOnCharacters) {
  EXPECT_EQ(Cut("abc", 5), "abc");
  EXPECT_EQ(Cut("abcdef", 3), "abc…");
  // "è" is two bytes: not cut in half.
  EXPECT_EQ(Cut("aè", 2), "a…");
  size_t next = 0;
  EXPECT_EQ(Part("abcdef", 0, 4, &next), "abcd");
  EXPECT_EQ(next, 4u);
  EXPECT_EQ(Part("abcdef", 4, 4, &next), "ef");
  EXPECT_EQ(next, 0u);
  // An offset inside a character moves to the next one.
  EXPECT_EQ(Part("èa", 1, 4, &next), "a");
  EXPECT_TRUE(IsTextMimeType("application/json"));
  EXPECT_TRUE(IsTextMimeType("text/html"));
  EXPECT_FALSE(IsTextMimeType("image/png"));
}

}  // namespace
}  // namespace compass::claude::devtools_format
