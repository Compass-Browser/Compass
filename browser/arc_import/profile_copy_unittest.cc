// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/profile_copy.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::arc {
namespace {

TEST(ChromiumMacCipherTest, RoundTripsAndRejectsOtherKeys) {
  ChromiumMacCipher arc("arc password");
  ChromiumMacCipher compass("compass password");
  const std::string encrypted = arc.Encrypt("secret cookie");
  EXPECT_TRUE(encrypted.starts_with("v10"));
  EXPECT_EQ(arc.Decrypt(encrypted), "secret cookie");
  EXPECT_NE(compass.Decrypt(encrypted), "secret cookie");
  EXPECT_FALSE(arc.Decrypt("plain"));
  // Same key, same IV: the same ciphertext, as Chromium's.
  EXPECT_EQ(ChromiumMacCipher("arc password").Encrypt("secret cookie"),
            encrypted);
}

class ProfileCopyTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    arc_ = dir_.GetPath().AppendASCII("Arc/Default");
    arc_cache_ = dir_.GetPath().AppendASCII("ArcCache/Default");
    compass_ = dir_.GetPath().AppendASCII("Compass/Profile 3");
    compass_cache_ = dir_.GetPath().AppendASCII("CompassCache/Profile 3");
    ASSERT_TRUE(
        base::CreateDirectory(arc_.AppendASCII("Local Storage/leveldb")));
    ASSERT_TRUE(base::WriteFile(
        arc_.AppendASCII("Local Storage/leveldb/000003.log"), "storage"));
    ASSERT_TRUE(base::WriteFile(arc_.AppendASCII("Preferences"), "{}"));
    ASSERT_TRUE(base::WriteFile(arc_.AppendASCII("LOCK"), ""));
    ASSERT_TRUE(base::CreateDirectory(arc_.AppendASCII("Sessions")));
    ASSERT_TRUE(
        base::WriteFile(arc_.AppendASCII("Sessions/Session_1"), "tabs"));
    ASSERT_TRUE(
        base::CreateDirectory(arc_cache_.AppendASCII("Cache/Cache_Data")));
    ASSERT_TRUE(base::WriteFile(
        arc_cache_.AppendASCII("Cache/Cache_Data/index"), "idx"));

    // Cookies: two encrypted with Arc's key, one Arc's key can't read, one
    // unencrypted.
    sql::Database cookies(sql::Database::Tag("Test"));
    ASSERT_TRUE(cookies.Open(arc_.AppendASCII("Cookies")));
    ASSERT_TRUE(cookies.Execute(
        "CREATE TABLE cookies (host_key TEXT, value TEXT, "
        "encrypted_value BLOB)"));
    InsertCookie(cookies, "a.test", arc_cipher_.Encrypt("session=1"));
    InsertCookie(cookies, "b.test", arc_cipher_.Encrypt("session=2"));
    InsertCookie(cookies, "c.test",
                 ChromiumMacCipher("other").Encrypt("session=3"));
    InsertCookie(cookies, "d.test", "");
    cookies.Close();

    sql::Database logins(sql::Database::Tag("Test"));
    ASSERT_TRUE(logins.Open(arc_.AppendASCII("Login Data")));
    ASSERT_TRUE(logins.Execute(
        "CREATE TABLE logins (origin_url TEXT, password_value BLOB)"));
    ASSERT_TRUE(logins.Execute(
        "CREATE TABLE meta (key TEXT PRIMARY KEY, value BLOB) WITHOUT ROWID"));
    {
      sql::Statement insert(logins.GetUniqueStatement(
          "INSERT INTO logins VALUES ('https://a.test/', ?)"));
      insert.BindBlob(0, arc_cipher_.Encrypt("hunter2"));
      ASSERT_TRUE(insert.Run());
    }
    logins.Close();
  }

  static void InsertCookie(sql::Database& db,
                           const std::string& host,
                           const std::string& encrypted) {
    sql::Statement insert(
        db.GetUniqueStatement("INSERT INTO cookies VALUES (?, '', ?)"));
    insert.BindString(0, host);
    insert.BindBlob(1, encrypted);
    ASSERT_TRUE(insert.Run());
  }

  CopyResult Copy() {
    return CopyProfile(
        arc_, compass_, arc_cache_, compass_cache_,
        base::BindRepeating(
            [](const ChromiumMacCipher* from, const ChromiumMacCipher* to,
               std::string_view value) -> std::optional<std::string> {
              std::optional<std::string> plain = from->Decrypt(value);
              return plain ? std::optional(to->Encrypt(*plain)) : std::nullopt;
            },
            &arc_cipher_, &compass_cipher_));
  }

  base::ScopedTempDir dir_;
  base::FilePath arc_, arc_cache_, compass_, compass_cache_;
  ChromiumMacCipher arc_cipher_{"arc password"};
  ChromiumMacCipher compass_cipher_{"compass password"};
};

TEST_F(ProfileCopyTest, CopiesFilesButNotLocksOrWindows) {
  ASSERT_TRUE(Copy().ok);
  EXPECT_TRUE(base::PathExists(compass_.AppendASCII("Preferences")));
  std::string storage;
  ASSERT_TRUE(base::ReadFileToString(
      compass_.AppendASCII("Local Storage/leveldb/000003.log"), &storage));
  EXPECT_EQ(storage, "storage");
  EXPECT_FALSE(base::PathExists(compass_.AppendASCII("LOCK")));
  EXPECT_FALSE(base::PathExists(compass_.AppendASCII("Sessions")));
  EXPECT_TRUE(
      base::PathExists(compass_cache_.AppendASCII("Cache/Cache_Data/index")));
  // Arc's own files are left as they were.
  EXPECT_TRUE(base::PathExists(arc_.AppendASCII("Sessions/Session_1")));
}

TEST_F(ProfileCopyTest, ReencryptsCookiesAndPasswords) {
  CopyResult result = Copy();
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.reencrypted, 3);
  EXPECT_EQ(result.dropped_cookies, 1);

  sql::Database cookies(sql::Database::Tag("Test"));
  ASSERT_TRUE(cookies.Open(compass_.AppendASCII("Cookies")));
  sql::Statement select(cookies.GetUniqueStatement(
      "SELECT host_key, encrypted_value FROM cookies ORDER BY host_key"));
  std::vector<std::pair<std::string, std::string>> rows;
  while (select.Step()) {
    rows.emplace_back(select.ColumnString(0), select.ColumnBlobAsString(1));
  }
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].first, "a.test");
  EXPECT_EQ(compass_cipher_.Decrypt(rows[0].second), "session=1");
  EXPECT_EQ(compass_cipher_.Decrypt(rows[1].second), "session=2");
  // The one Arc's key couldn't read is gone; the unencrypted one stays.
  EXPECT_EQ(rows[2].first, "d.test");

  sql::Database logins(sql::Database::Tag("Test"));
  ASSERT_TRUE(logins.Open(compass_.AppendASCII("Login Data")));
  sql::Statement password(
      logins.GetUniqueStatement("SELECT password_value FROM logins"));
  ASSERT_TRUE(password.Step());
  EXPECT_EQ(compass_cipher_.Decrypt(password.ColumnBlobAsString(0)),
            "hunter2");
}

TEST_F(ProfileCopyTest, NeverOverwrites) {
  ASSERT_TRUE(base::CreateDirectory(compass_));
  EXPECT_FALSE(Copy().ok);
}

}  // namespace
}  // namespace compass::arc
