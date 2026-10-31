// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/profile_copy.h"

#include <set>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace compass::arc {

namespace {

// Not copied: locks and logs, and Arc's open windows.
constexpr const char* kSkippedNames[] = {
    "LOCK",           "LOG",          "LOG.old",
    "Sessions",       "Sessions_Encrypted",
    "Current Session", "Current Tabs", "Last Session",
    "Last Tabs",      "SingletonLock", "SingletonCookie",
    "SingletonSocket", "RunningChromeVersion",
};

// The profile's databases with encrypted values.
constexpr const char* kEncryptedDatabases[] = {
    "Cookies",          "Login Data", "Login Data For Account",
    "Web Data",         "Account Web Data",
};

// Encrypted cookies Arc's key can't decrypt are left out (a cookie lost
// rather than a garbled one sent to a site).
constexpr char kCookiesTable[] = "cookies";

bool IsSkipped(const base::FilePath& path) {
  const std::string name = path.BaseName().AsUTF8Unsafe();
  for (const char* skipped : kSkippedNames) {
    if (name == skipped) {
      return true;
    }
  }
  return false;
}

// Copies |from| into |to| (created), recursively, without the skipped names
// at any level.
bool CopyTree(const base::FilePath& from, const base::FilePath& to) {
  if (!base::CreateDirectory(to)) {
    return false;
  }
  base::FileEnumerator entries(
      from, /*recursive=*/false,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
  for (base::FilePath entry = entries.Next(); !entry.empty();
       entry = entries.Next()) {
    if (IsSkipped(entry)) {
      continue;
    }
    const base::FilePath target = to.Append(entry.BaseName());
    if (entries.GetInfo().IsDirectory()) {
      if (!CopyTree(entry, target)) {
        return false;
      }
    } else if (!base::CopyFile(entry, target)) {
      LOG(ERROR) << "Arc import: can't copy " << entry;
      return false;
    }
  }
  return true;
}

std::vector<std::string> TableNames(sql::Database& db) {
  std::vector<std::string> tables;
  sql::Statement statement(db.GetUniqueStatement(
      "SELECT name FROM sqlite_master WHERE type='table' AND "
      "name NOT LIKE 'sqlite_%'"));
  while (statement.Step()) {
    tables.push_back(statement.ColumnString(0));
  }
  return tables;
}

std::vector<std::string> ColumnNames(sql::Database& db,
                                     const std::string& table) {
  std::vector<std::string> columns;
  sql::Statement statement(
      db.GetUniqueStatement("PRAGMA table_info(\"" + table + "\")"));
  while (statement.Step()) {
    columns.push_back(statement.ColumnString(1));
  }
  return columns;
}

// Re-encrypts every value of |db| that starts with "v10". Values that don't
// decrypt are left as they are, except cookies, which are deleted.
bool ReencryptDatabase(const base::FilePath& path,
                       const Reencrypt& reencrypt,
                       CopyResult& result) {
  // Upstream's tag for importing another browser's databases.
  sql::Database db(sql::DatabaseOptions(), "FirefoxImporter");
  if (!db.Open(path)) {
    LOG(ERROR) << "Arc import: can't open " << path;
    return false;
  }
  sql::Transaction transaction(&db);
  if (!transaction.Begin()) {
    return false;
  }
  for (const std::string& table : TableNames(db)) {
    if (table.find('"') != std::string::npos) {
      continue;
    }
    for (const std::string& column : ColumnNames(db, table)) {
      if (column.find('"') != std::string::npos) {
        continue;
      }
      const std::string quoted_table = "\"" + table + "\"";
      const std::string quoted_column = "\"" + column + "\"";
      // Rows first, then changes: the statement mustn't see its own updates.
      std::vector<std::pair<int64_t, std::string>> values;
      const std::string query =
          "SELECT rowid, " + quoted_column + " FROM " + quoted_table +
          " WHERE typeof(" + quoted_column + ")='blob' AND substr(" +
          quoted_column + ",1,3)=CAST('v10' AS BLOB)";
      // Tables without a rowid hold nothing Chromium encrypts.
      if (!db.IsSQLValid(query)) {
        continue;
      }
      {
        sql::Statement select(db.GetUniqueStatement(query));
        while (select.Step()) {
          values.emplace_back(select.ColumnInt64(0),
                              select.ColumnBlobAsString(1));
        }
      }
      for (const auto& [rowid, value] : values) {
        std::optional<std::string> converted = reencrypt.Run(value);
        if (converted) {
          sql::Statement update(db.GetUniqueStatement(
              "UPDATE " + quoted_table + " SET " + quoted_column +
              "=? WHERE rowid=?"));
          update.BindBlob(0, *converted);
          update.BindInt64(1, rowid);
          if (!update.Run()) {
            return false;
          }
          ++result.reencrypted;
        } else if (table == kCookiesTable) {
          sql::Statement remove(db.GetUniqueStatement(
              "DELETE FROM " + quoted_table + " WHERE rowid=?"));
          remove.BindInt64(0, rowid);
          if (!remove.Run()) {
            return false;
          }
          ++result.dropped_cookies;
        }
      }
    }
  }
  return transaction.Commit();
}

}  // namespace

CopyResult CopyProfile(const base::FilePath& from,
                       const base::FilePath& to,
                       const base::FilePath& from_cache,
                       const base::FilePath& to_cache,
                       const Reencrypt& reencrypt) {
  CopyResult result;
  if (!base::DirectoryExists(from) || base::PathExists(to)) {
    return result;
  }
  bool ok = CopyTree(from, to);
  for (const char* name : kEncryptedDatabases) {
    const base::FilePath database = to.Append(name);
    if (ok && base::PathExists(database)) {
      ok = ReencryptDatabase(database, reencrypt, result);
    }
  }
  // The cache is a cache: a failure to copy it doesn't fail the import.
  if (ok && base::DirectoryExists(from_cache) &&
      !base::PathExists(to_cache)) {
    if (!CopyTree(from_cache, to_cache)) {
      base::DeletePathRecursively(to_cache);
    }
  }
  if (!ok) {
    base::DeletePathRecursively(to);
    return CopyResult();
  }
  result.ok = true;
  return result;
}

}  // namespace compass::arc
