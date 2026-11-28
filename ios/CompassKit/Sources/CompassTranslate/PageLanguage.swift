// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

#if canImport(NaturalLanguage)
  import NaturalLanguage
#endif

/// A page's language and whether to offer translating it (docs/design/
/// translation.md, "iOS", "The page"). Codes are BCP 47 as the engines take
/// them: a language, and for Chinese its script (`zh-Hans`, `zh-Hant`).
public enum PageLanguage {
  /// `tag` as the engines take it: the language alone, lowercased, with
  /// old codes replaced (`iw` → `he`, `in` → `id`, `no` → `nb`), and Chinese
  /// as `zh-Hans` or `zh-Hant` (Taiwan, Hong Kong and Macau write
  /// traditional). nil for no language (`und`, `x-…`, empty).
  public static func normalize(_ tag: String) -> String? {
    let parts = tag.trimmingCharacters(in: .whitespacesAndNewlines)
      .replacingOccurrences(of: "_", with: "-")
      .split(separator: "-").map { $0.lowercased() }
    guard let first = parts.first, (2...3).contains(first.count), first.allSatisfy(\.isLetter),
      first != "und", first != "mul", first != "zxx"
    else { return nil }
    switch first {
    case "iw": return "he"
    case "in": return "id"
    case "ji": return "yi"
    case "no": return "nb"
    case "tl", "fil": return "tl"
    case "zh":
      let rest = Set(parts.dropFirst())
      if rest.contains("hant") || !rest.isDisjoint(with: ["tw", "hk", "mo"]) { return "zh-Hant" }
      return "zh-Hans"
    default: return first
    }
  }

  /// The language of a code without its script (`zh-Hant` → `zh`): what
  /// "a language the user reads" compares.
  public static func base(_ code: String) -> String {
    String(code.split(separator: "-").first ?? Substring(code))
  }

  /// The page's language from its `lang` attribute and a sample of its
  /// text: the text when it's clearly in one language (a template's
  /// leftover `lang="en"` on a page in German is common), else the
  /// attribute, else the text's best guess.
  public static func detect(langAttribute: String?, text: String) -> String? {
    let declared = langAttribute.flatMap(normalize)
    let guess = recognize(text)
    if let guess, guess.confidence >= 0.8 { return guess.code }
    if let declared { return declared }
    if let guess, guess.confidence >= 0.5 { return guess.code }
    return nil
  }

  /// The text's language and how sure the recognizer is; nil with too
  /// little text to tell (under 40 letters).
  public static func recognize(_ text: String) -> (code: String, confidence: Double)? {
    #if canImport(NaturalLanguage)
      guard text.filter(\.isLetter).count >= 40 else { return nil }
      let recognizer = NLLanguageRecognizer()
      recognizer.processString(String(text.prefix(4000)))
      guard let (language, confidence) = recognizer.languageHypotheses(withMaximum: 1).first,
        let code = normalize(language.rawValue)
      else { return nil }
      return (code, confidence)
    #else
      return nil
    #endif
  }

  /// The languages the user reads, from the system's preferred languages
  /// (`Locale.preferredLanguages`), as bases.
  public static func readLanguages(_ preferred: [String]) -> Set<String> {
    Set(preferred.compactMap(normalize).map(base))
  }

  /// Whether to offer translating a page in `source` (normalized): not one
  /// the user reads, and not one they said never to translate.
  public static func shouldOffer(source: String?, preferred: [String], never: Set<String>) -> Bool {
    guard let source else { return false }
    let base = base(source)
    return !readLanguages(preferred).contains(base) && !never.contains(source) && !never.contains(base)
  }

  /// The language to translate into: the first preferred language that
  /// isn't the page's.
  public static func target(source: String, preferred: [String]) -> String {
    let codes = preferred.compactMap(normalize)
    return codes.first { base($0) != base(source) } ?? (base(source) == "en" ? "it" : "en")
  }
}
