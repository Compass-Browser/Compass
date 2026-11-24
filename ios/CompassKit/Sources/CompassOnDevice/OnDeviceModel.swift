// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

#if canImport(FoundationModels)
  import FoundationModels
#endif

/// Apple Intelligence's on-device model for the sidebar's small requests
/// (docs/design/ai-sidebar.md, "On this device"): folder names, Tidy Today
/// and short tab titles. iOS calls it from `ClaudeQuick`; the Mac's
/// `compass-on-device` helper (tools/on_device/) is this library and a
/// `main.swift` that speaks `OnDeviceLines` to the browser.
///
/// Each feature's reply is generated as a fixed type (guided generation),
/// greedily, and handed back as the JSON object the feature's Claude prompt
/// asks for, so the callers' parsing (`parseTidyReply`, ParseTidyReply()...)
/// is the same for both.
public enum OnDeviceModel {
  /// The features it answers, by their prompt's id in
  /// common/claude/sidebar_prompts.md.
  public static let features: Set<String> = ["folder_name", "tidy_today", "short_titles"]

  /// Whether the model can answer now, and if not, why.
  public static var availability: OnDeviceAvailability {
    #if canImport(FoundationModels)
      if #available(macOS 26.0, iOS 26.0, *) {
        switch SystemLanguageModel.default.availability {
        case .available:
          return .available
        case .unavailable(.deviceNotEligible):
          return .unsupported
        case .unavailable(.appleIntelligenceNotEnabled):
          return .disabled
        case .unavailable(.modelNotReady):
          return .notReady
        case .unavailable:
          return .unsupported
        }
      }
    #endif
    return .unsupported
  }

  /// `feature`'s reply to `user`, told `system`: one JSON object, in the
  /// shape the feature's Claude prompt asks for.
  public static func answer(feature: String, system: String, user: String) async throws(OnDeviceFailure)
    -> String
  {
    guard features.contains(feature) else { throw .unknownFeature }
    #if canImport(FoundationModels)
      if #available(macOS 26.0, iOS 26.0, *) {
        guard availability == .available else { throw .unavailable }
        let reply: [String: Any]
        do {
          reply = try await OnDeviceGeneration.reply(feature: feature, system: system, user: user)
        } catch let error as OnDeviceFailure {
          throw error
        } catch {
          throw OnDeviceGeneration.failure(for: error)
        }
        guard
          let data = try? JSONSerialization.data(withJSONObject: reply, options: [.sortedKeys]),
          let text = String(data: data, encoding: .utf8)
        else { throw .failed }
        return text
      }
    #endif
    throw .unavailable
  }
}

/// The on-device model as its callers use it (`ClaudeQuick`, `OnDeviceLines`):
/// `.system` is Apple Intelligence; tests give their own.
public struct OnDeviceBackend: Sendable {
  public var availability: @Sendable () -> OnDeviceAvailability
  /// A feature's reply (feature, system prompt, user message).
  public var answer: @Sendable (String, String, String) async throws(OnDeviceFailure) -> String

  public init(
    availability: @escaping @Sendable () -> OnDeviceAvailability,
    answer: @escaping @Sendable (String, String, String) async throws(OnDeviceFailure) -> String
  ) {
    self.availability = availability
    self.answer = answer
  }

  public static let system = OnDeviceBackend(
    availability: { OnDeviceModel.availability },
    answer: { feature, system, user throws(OnDeviceFailure) in
      try await OnDeviceModel.answer(feature: feature, system: system, user: user)
    })
}

/// Whether the on-device model can answer.
public enum OnDeviceAvailability: String, Sendable, Equatable {
  case available
  /// An older system, or a device that can't run Apple Intelligence.
  case unsupported
  /// Apple Intelligence is turned off in the system's settings.
  case disabled
  /// The model is still downloading.
  case notReady = "not_ready"

  /// Why it can't answer, for the settings; nil while it can.
  public var reason: String? {
    switch self {
    case .available: nil
    case .unsupported: "This device or system version can't run Apple Intelligence."
    case .disabled: "Apple Intelligence is turned off in System Settings."
    case .notReady: "Apple Intelligence is still downloading. Try again later."
    }
  }
}

/// Why the on-device model gave no reply.
public enum OnDeviceFailure: String, Error, Sendable, Equatable {
  /// `OnDeviceModel.availability` isn't `.available`, or the model's assets
  /// went away.
  case unavailable
  /// The model's guardrails, or the model itself, declined.
  case refused
  /// Anything else: too long for the model, a language it doesn't speak,
  /// a reply that didn't decode.
  case failed
  /// A feature `OnDeviceModel.features` doesn't list.
  case unknownFeature = "unknown_feature"
}

#if canImport(FoundationModels)
  @available(macOS 26.0, iOS 26.0, *)
  enum OnDeviceGeneration {
    static func reply(feature: String, system: String, user: String) async throws -> [String: Any] {
      // Titles are the user's to rewrite: the guardrails for transforming
      // given text, not for writing new text.
      let model = SystemLanguageModel(guardrails: .permissiveContentTransformations)
      let session = LanguageModelSession(model: model, instructions: system)
      let options = GenerationOptions(samplingMode: .greedy)
      switch feature {
      case "folder_name":
        let name = try await session.respond(to: user, generating: FolderNameReply.self, options: options)
        return ["name": name.content.name]
      case "tidy_today":
        let tidy = try await session.respond(to: user, generating: TidyReply.self, options: options)
        return [
          "folders": tidy.content.folders.map { ["name": $0.name, "entries": $0.entries] as [String: Any] }
        ]
      case "short_titles":
        let short = try await session.respond(to: user, generating: ShortTitlesReply.self, options: options)
        var titles: [String: String] = [:]
        for item in short.content.titles where titles[item.id] == nil {
          titles[item.id] = item.title
        }
        return ["titles": titles]
      default:
        throw OnDeviceFailure.unknownFeature
      }
    }

    static func failure(for error: Error) -> OnDeviceFailure {
      guard let error = error as? LanguageModelSession.GenerationError else { return .failed }
      switch error {
      case .guardrailViolation, .refusal: return .refused
      case .assetsUnavailable: return .unavailable
      default: return .failed
      }
    }
  }

  @available(macOS 26.0, iOS 26.0, *)
  @Generable
  struct FolderNameReply {
    @Guide(description: "One to three words, or empty if the tabs have nothing in common")
    var name: String
  }

  @available(macOS 26.0, iOS 26.0, *)
  @Generable
  struct TidyReply {
    @Guide(.maximumCount(6))
    var folders: [TidyFolderReply]
  }

  @available(macOS 26.0, iOS 26.0, *)
  @Generable
  struct TidyFolderReply {
    @Guide(description: "One to three words")
    var name: String
    @Guide(description: "The entries' ids, exactly as listed", .minimumCount(2))
    var entries: [String]
  }

  @available(macOS 26.0, iOS 26.0, *)
  @Generable
  struct ShortTitlesReply {
    var titles: [ShortTitleReply]
  }

  @available(macOS 26.0, iOS 26.0, *)
  @Generable
  struct ShortTitleReply {
    @Guide(description: "The tab's id, exactly as listed")
    var id: String
    @Guide(description: "At most 40 characters")
    var title: String
  }
#endif
