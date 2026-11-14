// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SQLite3

/// One visit: this device's or one synced from another.
public struct Visit: Sendable, Equatable {
  /// When, in µs since the Windows epoch (Chromium's visit time; it is also
  /// the visit's sync tag, so it's unique per device and kept exact).
  public var time: Int64
  public var url: String
  public var title: String
  /// The cache GUID of the device that made it.
  public var originator: String
  /// The profile account it syncs in: empty for the home profile's, else
  /// the profile's UUID (each profile has its own history, as on the
  /// desktop).
  public var profile: String
  public var date: Date { Date(timeIntervalSince1970: TimeInterval(time - 11_644_473_600_000_000) / 1_000_000) }

  public init(time: Int64, url: String, title: String, originator: String, profile: String = "") {
    self.time = time
    self.url = url
    self.title = title
    self.originator = originator
    self.profile = profile
  }

  public static func time(_ date: Date) -> Int64 {
    Int64((date.timeIntervalSince1970 * 1_000_000).rounded(.down)) + 11_644_473_600_000_000
  }
}

/// A page in search results: the latest visit of a URL.
public struct HistoryEntry: Sendable, Equatable, Identifiable {
  public var id: String { url }
  public var url: String
  public var title: String
  public var lastVisit: Date
  public var visitCount: Int
}

/// History in SQLite, searched with FTS5. Not thread-safe: one owner
/// (the app's main actor).
public final class HistoryStore {
  private var db: OpaquePointer?
  public static let retention: TimeInterval = 90 * 24 * 60 * 60

  /// `path` nil = in memory.
  public init(path: String? = nil) throws {
    guard sqlite3_open(path ?? ":memory:", &db) == SQLITE_OK else { throw HistoryError.open }
    try exec(
      """
      PRAGMA journal_mode = WAL;
      CREATE TABLE IF NOT EXISTS visits (
        id INTEGER PRIMARY KEY,
        time INTEGER NOT NULL,
        originator TEXT NOT NULL,
        url TEXT NOT NULL,
        title TEXT NOT NULL,
        UNIQUE (originator, time)
      );
      CREATE INDEX IF NOT EXISTS visits_url ON visits (url);
      CREATE INDEX IF NOT EXISTS visits_time ON visits (time);
      CREATE VIRTUAL TABLE IF NOT EXISTS visits_fts USING fts5 (
        url, title, content = 'visits', content_rowid = 'id', tokenize = 'unicode61'
      );
      CREATE TRIGGER IF NOT EXISTS visits_ai AFTER INSERT ON visits BEGIN
        INSERT INTO visits_fts (rowid, url, title) VALUES (new.id, new.url, new.title);
      END;
      CREATE TRIGGER IF NOT EXISTS visits_ad AFTER DELETE ON visits BEGIN
        INSERT INTO visits_fts (visits_fts, rowid, url, title) VALUES ('delete', old.id, old.url, old.title);
      END;
      CREATE TRIGGER IF NOT EXISTS visits_au AFTER UPDATE ON visits BEGIN
        INSERT INTO visits_fts (visits_fts, rowid, url, title) VALUES ('delete', old.id, old.url, old.title);
        INSERT INTO visits_fts (rowid, url, title) VALUES (new.id, new.url, new.title);
      END;
      """)
    // M8: the profile account of each visit (old rows: the home's).
    let columns = query("PRAGMA table_info(visits)", []) { $0.text(1) }
    if !columns.contains("profile") {
      try exec("ALTER TABLE visits ADD COLUMN profile TEXT NOT NULL DEFAULT ''")
    }
  }

  deinit { sqlite3_close(db) }

  public enum HistoryError: Error { case open, sql(String) }

  /// Adds or updates a visit (same device and time = same visit).
  public func add(_ visit: Visit) {
    run(
      """
      INSERT INTO visits (time, originator, url, title, profile) VALUES (?, ?, ?, ?, ?)
      ON CONFLICT (originator, time) DO UPDATE SET url = excluded.url, title = excluded.title
      """, [.int(visit.time), .text(visit.originator), .text(visit.url), .text(visit.title), .text(visit.profile)])
  }

  public func visit(originator: String, time: Int64) -> Visit? {
    query(
      "SELECT time, url, title, originator, profile FROM visits WHERE originator = ? AND time = ?",
      [.text(originator), .int(time)]
    ) { Visit(time: $0.int(0), url: $0.text(1), title: $0.text(2), originator: $0.text(3), profile: $0.text(4)) }
    .first
  }

  /// Pages matching `text` (URL or title words), most recent first; with
  /// a profile, that profile account's only (the command bar, M8); with
  /// `since`, visited since then (Claude's search_history).
  public func search(_ text: String, limit: Int = 20, profile: String? = nil, since: Date? = nil) -> [HistoryEntry] {
    let words = text.lowercased().split { !$0.isLetter && !$0.isNumber }.map { "\"\($0)\"*" }
    guard !words.isEmpty else { return recent(limit: limit, profile: profile, since: since) }
    return query(
      """
      SELECT v.url, v.title, MAX(v.time), COUNT(*) FROM visits_fts f JOIN visits v ON v.id = f.rowid
      WHERE visits_fts MATCH ? AND (? OR v.profile = ?) AND v.time >= ?
      GROUP BY v.url ORDER BY MAX(v.time) DESC LIMIT ?
      """,
      [
        .text(words.joined(separator: " ")), .int(profile == nil ? 1 : 0), .text(profile ?? ""),
        .int(since.map(Visit.time) ?? 0), .int(Int64(limit)),
      ],
      entry)
  }

  public func recent(limit: Int = 50, profile: String? = nil, since: Date? = nil) -> [HistoryEntry] {
    query(
      """
      SELECT url, title, MAX(time), COUNT(*) FROM visits WHERE (? OR profile = ?) AND time >= ?
      GROUP BY url ORDER BY MAX(time) DESC LIMIT ?
      """,
      [.int(profile == nil ? 1 : 0), .text(profile ?? ""), .int(since.map(Visit.time) ?? 0), .int(Int64(limit))],
      entry)
  }

  public var count: Int {
    query("SELECT COUNT(*) FROM visits", []) { Int($0.int(0)) }.first ?? 0
  }

  /// Deletes visits between two times (Windows-epoch µs, inclusive); with
  /// a profile, only that profile's (a delete directive of its account).
  public func delete(from start: Int64, to end: Int64, profile: String? = nil) {
    run(
      "DELETE FROM visits WHERE time >= ? AND time <= ? AND (? OR profile = ?)",
      [.int(start), .int(end), .int(profile == nil ? 1 : 0), .text(profile ?? "")])
  }

  public func delete(url: String, until end: Int64, profile: String? = nil) {
    run(
      "DELETE FROM visits WHERE url = ? AND time <= ? AND (? OR profile = ?)",
      [.text(url), .int(end), .int(profile == nil ? 1 : 0), .text(profile ?? "")])
  }

  public func delete(times: [Int64], profile: String? = nil) {
    for t in times {
      run(
        "DELETE FROM visits WHERE time = ? AND (? OR profile = ?)",
        [.int(t), .int(profile == nil ? 1 : 0), .text(profile ?? "")])
    }
  }

  public func count(profile: String) -> Int {
    query("SELECT COUNT(*) FROM visits WHERE profile = ?", [.text(profile)]) { Int($0.int(0)) }.first ?? 0
  }

  public func deleteAll() { run("DELETE FROM visits", []) }

  public func expire(now: Date = Date()) {
    run("DELETE FROM visits WHERE time < ?", [.int(Visit.time(now.addingTimeInterval(-Self.retention)))])
  }

  // MARK: SQLite

  enum Value {
    case int(Int64)
    case text(String)
  }

  struct Row {
    let statement: OpaquePointer
    func int(_ i: Int32) -> Int64 { sqlite3_column_int64(statement, i) }
    func text(_ i: Int32) -> String { sqlite3_column_text(statement, i).map { String(cString: $0) } ?? "" }
  }

  private func entry(_ row: Row) -> HistoryEntry {
    HistoryEntry(
      url: row.text(0), title: row.text(1),
      lastVisit: Visit(time: row.int(2), url: "", title: "", originator: "").date, visitCount: Int(row.int(3)))
  }

  private func exec(_ sql: String) throws {
    var error: UnsafeMutablePointer<CChar>?
    if sqlite3_exec(db, sql, nil, nil, &error) != SQLITE_OK {
      let message = error.map { String(cString: $0) } ?? "?"
      sqlite3_free(error)
      throw HistoryError.sql(message)
    }
  }

  private func prepare(_ sql: String, _ values: [Value]) -> OpaquePointer? {
    var statement: OpaquePointer?
    guard sqlite3_prepare_v2(db, sql, -1, &statement, nil) == SQLITE_OK else { return nil }
    let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
    for (i, value) in values.enumerated() {
      switch value {
      case .int(let v): sqlite3_bind_int64(statement, Int32(i + 1), v)
      case .text(let v): sqlite3_bind_text(statement, Int32(i + 1), v, -1, transient)
      }
    }
    return statement
  }

  private func run(_ sql: String, _ values: [Value]) {
    guard let statement = prepare(sql, values) else { return }
    sqlite3_step(statement)
    sqlite3_finalize(statement)
  }

  private func query<T>(_ sql: String, _ values: [Value], _ map: (Row) -> T) -> [T] {
    guard let statement = prepare(sql, values) else { return [] }
    defer { sqlite3_finalize(statement) }
    var out: [T] = []
    while sqlite3_step(statement) == SQLITE_ROW { out.append(map(Row(statement: statement))) }
    return out
  }
}
