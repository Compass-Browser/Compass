// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// How the developer tools (docs/design/claude.md, "Developer tools") write
/// what they found for Claude, as the desktop's devtools_format: sizes,
/// times, headers, and secrets hidden unless the user asked to see them.
public enum DevToolsFormat {
  /// Parts of names that carry credentials. "auth" covers Authorization and
  /// x-auth-*; "passw" password and passwd.
  static let secretParts = [
    "token", "secret", "passw", "pwd", "session", "auth", "credential", "apikey", "api-key", "api_key",
    "private-key", "private_key", "jwt", "signature", "cookie",
  ]
  static let secretPattern =
    "(?:token|secret|passw|pwd|session|auth|credential|api[-_]?key|private[-_]?key|jwt|signature|cookie)"

  // NSRegularExpression is thread-safe once made.
  private static let formField = try! NSRegularExpression(
    pattern: "((?:^|[?&;])[^=&;?/\\s]*\(secretPattern)[^=&;?/\\s]*=)[^&;\\s#]*", options: [.caseInsensitive])
  private static let jsonMember = try! NSRegularExpression(
    pattern: "(\"[^\"\\\\]*\(secretPattern)[^\"\\\\]*\"\\s*:\\s*)\"(?:[^\"\\\\]|\\\\.)*\"", options: [.caseInsensitive])
  private static let jwt = try! NSRegularExpression(
    pattern: "eyJ[A-Za-z0-9_-]{5,}\\.[A-Za-z0-9_-]{5,}\\.[A-Za-z0-9_-]*")

  /// Whether a header, cookie, storage key or form field named `name`
  /// carries credentials.
  public static func isSecretName(_ name: String) -> Bool {
    let lower = name.lowercased()
    return secretParts.contains { lower.contains($0) }
  }

  /// Whether `value` looks like a credential whatever its name: a JWT, a
  /// "Bearer …" or "Basic …" value.
  public static func looksSecret(_ value: String) -> Bool {
    let trimmed = value.trimmingCharacters(in: .whitespaces)
    let lower = trimmed.lowercased()
    if lower.hasPrefix("bearer ") || lower.hasPrefix("basic ") { return true }
    return jwt.firstMatch(in: trimmed, range: NSRange(trimmed.startIndex..., in: trimmed)) != nil
  }

  /// What stands for a hidden value: "<hidden, 32 characters>".
  public static func hidden(_ value: String) -> String {
    let count = value.utf16.count
    return "<hidden, \(count) \(count == 1 ? "character" : "characters")>"
  }

  /// `text` with the values of secret-named fields hidden: form and query
  /// fields ("access_token=…") and JSON members ("password": "…"), and JWTs.
  public static func maskSecretsInText(_ text: String) -> String {
    var masked = text
    for (regex, template) in [(formField, "$1<hidden>"), (jsonMember, "$1\"<hidden>\""), (jwt, "<hidden JWT>")] {
      masked = regex.stringByReplacingMatches(
        in: masked, range: NSRange(masked.startIndex..., in: masked), withTemplate: template)
    }
    return masked
  }

  /// Headers as "  name: value" lines, sorted by name; credential values
  /// hidden unless `showSecrets`.
  public static func formatHeaders(_ headers: [(String, String)], showSecrets: Bool) -> String {
    var lines: [(name: String, value: String)] = []
    for (name, value) in headers {
      let secret = !showSecrets && (isSecretName(name) || looksSecret(value))
      lines.append((name.lowercased(), secret ? hidden(value) : cut(value, 500)))
    }
    lines.sort { $0.name == $1.name ? $0.value < $1.value : $0.name < $1.name }
    var out = ""
    for line in lines { out += "  \(line.name): \(line.value)\n" }
    return out
  }

  /// "512 B", "3.2 kB", "1.4 MB".
  public static func formatBytes(_ bytes: Double) -> String {
    if bytes < 1000 { return "\(Int(bytes)) B" }
    if bytes < 1_000_000 { return String(format: "%.1f kB", bytes / 1000) }
    return String(format: "%.1f MB", bytes / 1_000_000)
  }

  /// "120 ms", "1.4 s".
  public static func formatDuration(_ seconds: Double) -> String {
    seconds < 1 ? "\(Int(seconds * 1000 + 0.5)) ms" : String(format: "%.1f s", seconds)
  }

  /// A timing in milliseconds, or "unavailable" when the page has none (-1).
  public static func ms(_ milliseconds: Double) -> String {
    milliseconds < 0 ? "unavailable" : formatDuration(milliseconds / 1000)
  }

  /// "good", "needs improvement" or "poor", by web.dev's thresholds.
  public static func rate(_ value: Double, good: Double, poor: Double) -> String {
    value <= good ? "good" : value <= poor ? "needs improvement" : "poor"
  }

  /// `text` cut to at most `max` characters, with "…" when cut.
  public static func cut(_ text: String, _ max: Int) -> String {
    text.count <= max ? text : String(text.prefix(max)) + "…"
  }

  /// The part of `text` from character `offset`, at most `max` long, and
  /// where the next part starts (nil when this is the last).
  public static func part(_ text: String, offset: Int, max: Int) -> (text: String, next: Int?) {
    let start = text.index(text.startIndex, offsetBy: min(Swift.max(0, offset), text.count))
    let rest = text[start...]
    guard rest.count > max else { return (String(rest), nil) }
    return (String(rest.prefix(max)), offset + max)
  }

  /// Whether a body of `mimeType` is text Claude can read.
  public static func isTextMimeType(_ mimeType: String) -> Bool {
    let lower = mimeType.lowercased()
    if lower.hasPrefix("text/") { return true }
    return ["json", "javascript", "ecmascript", "xml", "x-www-form-urlencoded", "graphql", "csv", "yaml"]
      .contains { lower.contains($0) }
  }

  /// A cookie as page_storage lists it (the desktop's FormatCookie).
  public struct Cookie: Sendable, Equatable {
    public var name: String
    public var value: String
    public var domain: String
    public var path: String
    /// Nil for a session cookie.
    public var expires: Date?
    public var httpOnly: Bool
    public var secure: Bool
    public var sameSite: String?

    public init(
      name: String, value: String, domain: String, path: String, expires: Date?, httpOnly: Bool, secure: Bool,
      sameSite: String?
    ) {
      self.name = name
      self.value = value
      self.domain = domain
      self.path = path
      self.expires = expires
      self.httpOnly = httpOnly
      self.secure = secure
      self.sameSite = sameSite
    }

    /// Whether the cookie goes with requests to `host` (domain matching).
    public func applies(to host: String) -> Bool {
      let domain = self.domain.hasPrefix(".") ? String(self.domain.dropFirst()) : self.domain
      let host = host.lowercased()
      return host == domain.lowercased() || host.hasSuffix("." + domain.lowercased())
    }

    public func line(showValues: Bool) -> String {
      var text = "  \(name)=\(showValues ? cut(value, 500) : hidden(value)); domain \(domain); path \(path)"
      if let expires {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyy-MM-dd"
        text += "; expires \(formatter.string(from: expires))"
      } else {
        text += "; session"
      }
      if httpOnly { text += "; HttpOnly" }
      if secure { text += "; Secure" }
      if let sameSite { text += "; SameSite=\(sameSite)" }
      return text + "\n"
    }
  }
}
