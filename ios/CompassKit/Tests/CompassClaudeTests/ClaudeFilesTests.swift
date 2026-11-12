// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassClaude

/// The file tools' rules (docs/design/claude.md, "Files"), in a temporary
/// folder.
struct ClaudeFilesTests {
  private func folder() throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("claude-files-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
  }

  @Test func names() {
    #expect(ClaudeFiles.isValidName("notes.md"))
    #expect(ClaudeFiles.isValidName("Report 2026.csv"))
    #expect(!ClaudeFiles.isValidName(""))
    #expect(!ClaudeFiles.isValidName(".hidden"))
    #expect(!ClaudeFiles.isValidName("a/b.md"))
    #expect(!ClaudeFiles.isValidName("a\\b.md"))
    #expect(!ClaudeFiles.isValidName(".."))
    #expect(!ClaudeFiles.isValidName(String(repeating: "a", count: 201)))
  }

  @Test func paths() {
    #expect(ClaudeFiles.parts("") == [])
    #expect(ClaudeFiles.parts("notes/today.md") == ["notes", "today.md"])
    #expect(ClaudeFiles.parts("/etc/passwd") == nil)
    #expect(ClaudeFiles.parts("~/x") == nil)
    #expect(ClaudeFiles.parts("C:/x") == nil)
    #expect(ClaudeFiles.parts("a/../b") == nil)
    #expect(ClaudeFiles.parts("./a") == nil)
    #expect(ClaudeFiles.parts("a//b") == nil)
    #expect(ClaudeFiles.parts("a/") == nil)
    #expect(ClaudeFiles.parts("a\\b") == nil)
  }

  @Test func saveNeverReplaces() throws {
    let root = try folder()
    let files = ClaudeFiles(root: root)
    let first = try files.save(name: "notes.md", content: "one")
    let second = try files.save(name: "notes.md", content: "two")
    let third = try files.save(name: "README", content: "three")
    let fourth = try files.save(name: "README", content: "four")
    #expect(first.lastPathComponent == "notes.md")
    #expect(second.lastPathComponent == "notes (1).md")
    #expect(third.lastPathComponent == "README")
    #expect(fourth.lastPathComponent == "README (1)")
    #expect(try String(contentsOf: first, encoding: .utf8) == "one")
    #expect(try String(contentsOf: second, encoding: .utf8) == "two")
    #expect(throws: ClaudeFiles.Failure.badName) { try files.save(name: "../x", content: "") }
  }

  @Test func writeReadAndList() throws {
    let root = try folder()
    let files = ClaudeFiles(root: root)
    let target = try files.target("notes/today.md")
    #expect(!target.replaces)
    try files.write("hello", to: target.url)
    #expect(try files.target("notes/today.md").replaces)
    #expect(try files.read("notes/today.md", offset: 0) == "hello")
    #expect(try files.read("notes/today.md", offset: 2) == "llo")
    let listing = try files.list("")
    #expect(listing.hasPrefix("notes/"))
    #expect(try files.list("notes").hasPrefix("today.md  5 bytes"))
    #expect(throws: ClaudeFiles.Failure.isAFolder("notes")) { try files.target("notes") }
    #expect(throws: ClaudeFiles.Failure.notFound("missing.md")) { try files.read("missing.md", offset: 0) }
    #expect(throws: ClaudeFiles.Failure.badPath("../x")) { try files.read("../x", offset: 0) }
  }

  @Test func symlinksDontLeadOut() throws {
    let root = try folder()
    let outside = try folder()
    try "secret".write(to: outside.appendingPathComponent("secret.txt"), atomically: true, encoding: .utf8)
    try FileManager.default.createSymbolicLink(
      at: root.appendingPathComponent("link"), withDestinationURL: outside)
    let files = ClaudeFiles(root: root)
    #expect(throws: ClaudeFiles.Failure.outside) { try files.read("link/secret.txt", offset: 0) }
    #expect(throws: ClaudeFiles.Failure.outside) { try files.target("link/new.txt") }
    // A link inside the folder is fine.
    try FileManager.default.createDirectory(
      at: root.appendingPathComponent("real"), withIntermediateDirectories: true)
    try FileManager.default.createSymbolicLink(
      at: root.appendingPathComponent("alias"), withDestinationURL: root.appendingPathComponent("real"))
    let target = try files.target("alias/a.txt")
    try files.write("ok", to: target.url)
    #expect(try files.read("real/a.txt", offset: 0) == "ok")
  }

  @Test func textOnly() throws {
    let root = try folder()
    try Data([0x89, 0x50, 0x4e, 0x47, 0x00, 0x01]).write(to: root.appendingPathComponent("a.png"))
    try Data([0xff, 0xfe, 0x41]).write(to: root.appendingPathComponent("b.txt"))
    let files = ClaudeFiles(root: root)
    #expect(throws: ClaudeFiles.Failure.notText("a.png", 6)) { try files.read("a.png", offset: 0) }
    #expect(throws: ClaudeFiles.Failure.notText("b.txt", 3)) { try files.read("b.txt", offset: 0) }
  }

  @Test func longFiles() {
    let text = String(repeating: "a", count: ClaudeFiles.readChars + 10)
    let first = ClaudeFiles.slice(text, offset: 0)
    #expect(first.hasPrefix(String(repeating: "a", count: ClaudeFiles.readChars) + "\n…10 more characters"))
    #expect(ClaudeFiles.slice(text, offset: ClaudeFiles.readChars) == String(repeating: "a", count: 10))
    #expect(ClaudeFiles.slice("abc", offset: 99) == "")
    #expect(ClaudeFiles.preview(String(repeating: "b", count: 3000)).count == ClaudeFiles.previewChars + 2)
  }

  @Test func contextLines() {
    #expect(ClaudeFiles.contextLine(.off, folderName: "", folderPath: "") == "File access is off: the file tools refuse.")
    #expect(ClaudeFiles.contextLine(.save, folderName: "Downloads", folderPath: "").contains("into Downloads"))
    #expect(ClaudeFiles.contextLine(.folder, folderName: "", folderPath: "/x/Projects").contains("folder /x/Projects"))
  }
}
