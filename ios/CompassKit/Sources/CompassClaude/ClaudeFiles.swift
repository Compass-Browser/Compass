// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// What Claude may do with files (Settings → Claude → Files;
/// docs/design/claude.md, "Files"): the desktop's
/// `compass.claude.files_access`.
public enum ClaudeFileAccess: Int, CaseIterable, Sendable {
  case off = 0
  /// save_file only.
  case save = 1
  /// save_file, list_files, read_file and write_file in one folder.
  case folder = 2
}

/// The file tools' rules and file work, as the desktop's
/// browser/claude/claude_files.cc: paths stay in the folder (symlinks
/// resolved), text only, new files never replace one.
public struct ClaudeFiles: Sendable {
  /// A file's text, at most.
  public static let maxChars = 1_000_000
  /// read_file gives this much at a time.
  public static let readChars = 100_000
  /// list_files lists this many entries at most.
  public static let maxEntries = 500
  /// The approval shows this much of the content.
  public static let previewChars = 2000
  /// A saved file's name, at most.
  public static let maxNameLength = 200

  public static let notPermittedOff =
    "Not permitted: the user hasn't turned on file access (Compass settings, Claude, Files). Tell the user if the task needs it."
  public static let notPermittedSaveOnly =
    "Not permitted: Claude can only save new files. \"Use a Folder\" in Compass's Claude settings (Files) lets Claude list, read and write files in one folder; tell the user if the task needs it."
  public static let notPermittedNoFolder =
    "Not permitted: no folder is chosen for Claude (Compass settings, Claude, Files). Tell the user if the task needs it."
  public static let denied = "The user didn't allow it."

  public enum Failure: Error, Equatable, CustomStringConvertible, Sendable {
    case badName
    case badPath(String)
    case outside
    case notFound(String)
    case notAFolder(String)
    case isAFolder(String)
    case notText(String, Int)
    case tooLong
    case io(String)

    public var description: String {
      switch self {
      case .badName:
        return "Give a file name only, like \"notes.md\": no folders, not starting with a dot, at most \(ClaudeFiles.maxNameLength) characters."
      case .badPath(let path):
        return "\"\(path)\" isn't a path inside the folder: give one relative to it, like \"notes/today.md\", without \"..\"."
      case .outside: return "That path leads outside the folder."
      case .notFound(let path): return "There's no \"\(path)\" in the folder."
      case .notAFolder(let path): return "\"\(path)\" isn't a folder."
      case .isAFolder(let path): return "\"\(path)\" is a folder, not a file."
      case .notText(let path, let size): return "\"\(path)\" isn't a text file (\(size) bytes)."
      case .tooLong: return "The content is too long: at most \(ClaudeFiles.maxChars) characters a file."
      case .io(let text): return text
      }
    }
  }

  /// The folder the tools work in.
  public let root: URL

  public init(root: URL) { self.root = root }

  /// The context line for the user's message (the desktop's
  /// BuildMessageContext()).
  public static func contextLine(_ access: ClaudeFileAccess, folderName: String, folderPath: String) -> String {
    switch access {
    case .off: return "File access is off: the file tools refuse."
    case .save: return "You can save new files for the user with save_file, into \(folderName); each save asks the user."
    case .folder:
      return "You can list, read and write files in the user's folder \(folderPath) (paths are relative to it); each save or write asks the user."
    }
  }

  // MARK: Names and paths

  /// A save_file name: a file name only.
  public static func isValidName(_ name: String) -> Bool {
    !name.isEmpty && name.count <= maxNameLength && !name.hasPrefix(".") && !name.contains("/")
      && !name.contains("\\") && !name.contains(":") && !name.contains("\0")
      && name.trimmingCharacters(in: .whitespaces) == name
  }

  /// A relative path's parts, or nil: no absolute path, drive letter,
  /// backslash, `.`, `..` or empty part. Empty is the folder itself.
  public static func parts(_ path: String) -> [String]? {
    if path.isEmpty { return [] }
    if path.hasPrefix("/") || path.hasPrefix("~") || path.contains("\\") || path.contains("\0") { return nil }
    if path.count >= 2, path[path.index(after: path.startIndex)] == ":" { return nil }
    let parts = path.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
    for part in parts where part.isEmpty || part == "." || part == ".." { return nil }
    return parts
  }

  /// `path` in the folder, its real path (symlinks resolved) inside the
  /// folder's, or the nearest existing parent's for a new one.
  public func resolve(_ path: String) throws(Failure) -> URL {
    guard let parts = Self.parts(path) else { throw .badPath(path) }
    var url = root
    for part in parts { url.appendPathComponent(part) }
    let realRoot = root.resolvingSymlinksInPath().standardizedFileURL.path
    var existing = url
    var rest: [String] = []
    while !FileManager.default.fileExists(atPath: existing.path), existing.path != root.path {
      rest.insert(existing.lastPathComponent, at: 0)
      existing.deleteLastPathComponent()
    }
    var real = existing.resolvingSymlinksInPath().standardizedFileURL
    let realPath = real.path
    guard realPath == realRoot || realPath.hasPrefix(realRoot.hasSuffix("/") ? realRoot : realRoot + "/") else {
      throw .outside
    }
    for part in rest { real.appendPathComponent(part) }
    return real
  }

  /// "notes.md", else "notes (1).md", "notes (2).md"… (Chromium's
  /// GetUniquePath()).
  public static func uniqueURL(_ name: String, in folder: URL) -> URL? {
    let first = folder.appendingPathComponent(name)
    if !FileManager.default.fileExists(atPath: first.path) { return first }
    let ext = (name as NSString).pathExtension
    let base = ext.isEmpty ? name : String(name.dropLast(ext.count + 1))
    for n in 1...100 {
      let candidate = folder.appendingPathComponent(ext.isEmpty ? "\(base) (\(n))" : "\(base) (\(n)).\(ext)")
      if !FileManager.default.fileExists(atPath: candidate.path) { return candidate }
    }
    return nil
  }

  // MARK: Text

  /// UTF-8 text without NUL bytes, or nil.
  public static func text(_ data: Data) -> String? {
    if data.contains(0) { return nil }
    return String(data: data, encoding: .utf8)
  }

  /// readChars characters from `offset`, with what's left.
  public static func slice(_ text: String, offset: Int) -> String {
    let count = text.count
    let start = min(max(offset, 0), count)
    let startIndex = text.index(text.startIndex, offsetBy: start)
    let taken = min(readChars, count - start)
    let endIndex = text.index(startIndex, offsetBy: taken)
    var result = String(text[startIndex..<endIndex])
    let left = count - start - taken
    if left > 0 {
      result += "\n…\(left) more characters (read_file with offset \(start + readChars))."
    }
    return result
  }

  /// The approval's preview of `content`.
  public static func preview(_ content: String) -> String {
    content.count <= previewChars ? content : String(content.prefix(previewChars)) + "\n…"
  }

  // MARK: The tools' file work

  /// save_file: a new file in the root, never replacing one. Returns it.
  public func save(name: String, content: String) throws(Failure) -> URL {
    guard Self.isValidName(name) else { throw .badName }
    guard content.count <= Self.maxChars else { throw .tooLong }
    do {
      try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    } catch { throw .io("Couldn't make the folder: \(error.localizedDescription)") }
    guard let url = Self.uniqueURL(name, in: root) else { throw .io("Too many files named \(name).") }
    do {
      try Data(content.utf8).write(to: url, options: .withoutOverwriting)
    } catch { throw .io("Couldn't save the file: \(error.localizedDescription)") }
    return url
  }

  /// list_files.
  public func list(_ path: String) throws(Failure) -> String {
    let url = try resolve(path)
    var isFolder: ObjCBool = false
    guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isFolder) else {
      throw .notFound(path)
    }
    guard isFolder.boolValue else { throw .notAFolder(path) }
    let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey]
    let items: [URL]
    do {
      items = try FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: keys)
    } catch { throw .io("Couldn't list the folder: \(error.localizedDescription)") }
    let sorted = items.sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    if sorted.isEmpty { return "\(path.isEmpty ? "The folder" : "\"\(path)\"") is empty." }
    let formatter = ISO8601DateFormatter()
    var lines: [String] = []
    for item in sorted.prefix(Self.maxEntries) {
      let values = try? item.resourceValues(forKeys: Set(keys))
      let date = values?.contentModificationDate.map { formatter.string(from: $0) } ?? ""
      if values?.isDirectory == true {
        lines.append("\(item.lastPathComponent)/  \(date)")
      } else {
        lines.append("\(item.lastPathComponent)  \(values?.fileSize ?? 0) bytes  \(date)")
      }
    }
    if sorted.count > Self.maxEntries { lines.append("…and \(sorted.count - Self.maxEntries) more.") }
    return lines.joined(separator: "\n")
  }

  /// read_file.
  public func read(_ path: String, offset: Int) throws(Failure) -> String {
    let url = try resolve(path)
    var isFolder: ObjCBool = false
    guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isFolder) else { throw .notFound(path) }
    guard !isFolder.boolValue else { throw .isAFolder(path) }
    let data: Data
    do { data = try Data(contentsOf: url) } catch { throw .io("Couldn't read the file: \(error.localizedDescription)") }
    guard let text = Self.text(data) else { throw .notText(path, data.count) }
    return Self.slice(text, offset: offset)
  }

  /// write_file's target, and whether it replaces a file (asked before
  /// writing).
  public func target(_ path: String) throws(Failure) -> (url: URL, replaces: Bool) {
    guard !path.isEmpty else { throw .badPath(path) }
    let url = try resolve(path)
    var isFolder: ObjCBool = false
    let exists = FileManager.default.fileExists(atPath: url.path, isDirectory: &isFolder)
    if exists && isFolder.boolValue { throw .isAFolder(path) }
    return (url, exists)
  }

  /// write_file, once approved: the folders on the way, then the file.
  public func write(_ content: String, to url: URL) throws(Failure) {
    guard content.count <= Self.maxChars else { throw .tooLong }
    do {
      try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
      try Data(content.utf8).write(to: url, options: .atomic)
    } catch { throw .io("Couldn't write the file: \(error.localizedDescription)") }
  }
}
