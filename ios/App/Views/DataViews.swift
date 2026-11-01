// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI
import WebKit

/// Settings → Clear Browsing Data: history, cookies and site data, and the
/// cache, for a time range, in every profile.
struct ClearDataView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss

  enum Range: String, CaseIterable, Identifiable {
    case hour = "Last Hour"
    case day = "Today"
    case week = "Last 7 Days"
    case all = "All Time"

    var id: String { rawValue }

    /// nil: everything.
    var since: Date? {
      switch self {
      case .hour: Date().addingTimeInterval(-3600)
      case .day: Calendar.current.startOfDay(for: Date())
      case .week: Date().addingTimeInterval(-7 * 24 * 3600)
      case .all: nil
      }
    }
  }

  @State private var range = Range.hour
  @State private var history = true
  @State private var cookies = true
  @State private var cache = true
  @State private var confirming = false
  @State private var clearing = false

  var body: some View {
    Form {
      Section {
        Picker("Time Range", selection: $range) {
          ForEach(Range.allCases) { Text($0.rawValue).tag($0) }
        }
      }
      Section {
        Toggle("History", isOn: $history)
        Toggle("Cookies and Site Data", isOn: $cookies)
        Toggle("Cached Images and Files", isOn: $cache)
      } footer: {
        Text(footer)
      }
      Section {
        Button("Clear Browsing Data", role: .destructive) { confirming = true }
          .disabled(!(history || cookies || cache) || clearing)
      }
    }
    .navigationTitle("Clear Browsing Data")
    .confirmationDialog("Clear browsing data from \(range.rawValue.lowercased())?", isPresented: $confirming,
                        titleVisibility: .visible) {
      Button("Clear", role: .destructive) { Task { await clear() } }
    }
  }

  private var footer: String {
    var parts: [String] = []
    if history { parts.append(app.sync.isOn ? "History is cleared on every synced device." : "History is cleared on this iPhone.") }
    if cookies { parts.append("Clearing cookies signs you out of websites, in every profile.") }
    return parts.joined(separator: " ")
  }

  private func clear() async {
    clearing = true
    if history { app.sync.clearHistory(since: range.since) }
    var types: Set<String> = []
    if cookies { types.formUnion(WKWebsiteDataStore.allWebsiteDataTypes().subtracting(Self.cacheTypes)) }
    if cache { types.formUnion(Self.cacheTypes) }
    if !types.isEmpty {
      for store in app.tabs.allDataStores() {
        await store.removeData(ofTypes: types, modifiedSince: range.since ?? .distantPast)
      }
    }
    clearing = false
    dismiss()
  }

  static let cacheTypes: Set<String> = [
    WKWebsiteDataTypeDiskCache, WKWebsiteDataTypeMemoryCache, WKWebsiteDataTypeFetchCache,
    WKWebsiteDataTypeOfflineWebApplicationCache,
  ]
}

/// How long Today's tabs stay before they move to the archive. It's each
/// space's own setting, synced with the desktop.
enum ArchiveAfter: CaseIterable, Identifiable {
  case twelveHours, day, week, month, never

  var id: Self { self }

  var interval: TimeInterval? {
    switch self {
    case .twelveHours: 12 * 3600
    case .day: 24 * 3600
    case .week: 7 * 24 * 3600
    case .month: 30 * 24 * 3600
    case .never: nil
    }
  }

  var name: String {
    switch self {
    case .twelveHours: "After 12 Hours"
    case .day: "After 1 Day"
    case .week: "After 7 Days"
    case .month: "After 30 Days"
    case .never: "Never"
    }
  }

  /// Every space's, or nil when they differ.
  static func current(_ spaces: [Space]) -> ArchiveAfter? {
    let choices = Set(spaces.map { ArchiveAfter($0.archiveAfter) })
    return choices.count == 1 ? choices.first : nil
  }

  init(_ interval: TimeInterval?) {
    self = Self.allCases.first { $0.interval == (interval ?? 0 > 0 ? interval : nil) } ?? .twelveHours
  }
}

/// Settings → Archive Inactive Tabs: one choice for every space.
struct ArchiveAfterPicker: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    List {
      ForEach(ArchiveAfter.allCases) { choice in
        Button {
          for space in app.store.spaces { app.store.setArchiveAfter(space, choice.interval) }
        } label: {
          HStack {
            Text(choice.name).foregroundStyle(.primary)
            Spacer()
            if current == choice { Image(systemName: "checkmark").foregroundStyle(Color.accentColor) }
          }
        }
      }
    }
    .navigationTitle("Archive Inactive Tabs")
  }

  var current: ArchiveAfter? { ArchiveAfter.current(app.store.spaces) }
}

extension AppModel {
  /// Every space's Pinned and Today tabs as text, grouped by space, for
  /// Settings → Export Open Tabs.
  var openTabsText: String {
    store.spaces.map { space in
      let tabs = (space.pinned.children + space.today.children.reversed())
        .flatMap(\.subtree).filter { $0.isTab && PhoneVisible.url($0.url) }
      let lines = tabs.map { "- \($0.displayTitle): \($0.url)" }
      return (["\(space.name)"] + lines).joined(separator: "\n")
    }.joined(separator: "\n\n")
  }
}
