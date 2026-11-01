// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

/// Site Settings for one site (from the page menu), or the defaults for
/// every site (`site` nil, Global Site Settings).
struct SiteSettingsView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  let site: String?

  var body: some View {
    NavigationStack {
      Form {
        Section(site.map { "\($0) settings" } ?? "Every site") {
          toggle(.blockAds, "Block Ads and Trackers", "hand.raised.fill", .red)
          toggle(.blockCookieBanners, "Block Cookie Banners", "checkmark.shield.fill", .orange)
        }
        Section {
          toggle(.collapseBar, "Collapse Bottom Bar", "rectangle.bottomhalf.inset.filled", .purple)
          toggle(.swipeToNavigate, "Swipe to Navigate", "hand.draw.fill", .green)
          toggle(.desktopSite, "Desktop Site", "desktopcomputer", .blue)
        } footer: {
          if site != nil {
            Text("Changes here apply to this site only. Defaults for every site are in Settings.")
          }
        }
        if site == nil { filterLists }
      }
      .navigationTitle(site == nil ? "Global Site Settings" : "Site Settings")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        if site != nil { Button("Done") { dismiss() } }
      }
    }
  }

  /// The lists blocking uses: rules, last update, and Update Now.
  @ViewBuilder private var filterLists: some View {
    let blocking = app.contentBlocking
    Section {
      ForEach(FilterList.allCases, id: \.self) { list in
        let status = blocking.status[list]
        VStack(alignment: .leading, spacing: 2) {
          HStack {
            Text(list.name)
            Spacer()
            if status?.working == true { ProgressView() }
          }
          Group {
            if let error = status?.error {
              Text(error).foregroundStyle(.red)
            } else if let rules = status?.rules, let updated = status?.updated {
              Text("\(rules.formatted()) rules · updated \(updated.formatted(.relative(presentation: .named)))")
            } else {
              Text("Not downloaded yet")
            }
          }
          .font(.caption)
          .foregroundStyle(.secondary)
        }
      }
      Button("Update Now") { Task { await blocking.refresh(force: true) } }
        .disabled(FilterList.allCases.contains { blocking.status[$0]?.working == true })
    } header: {
      Text("Filter lists")
    } footer: {
      Text("Downloaded from their publishers and checked for updates once a day.")
    }
  }

  private func toggle(_ option: SiteSettings.Option, _ title: String, _ symbol: String, _ color: Color) -> some View {
    Toggle(isOn: Binding(
      get: { app.siteSettings.value(option, site: site) },
      set: { app.siteSettings.set(option, $0, site: site) })
    ) {
      HStack(spacing: 12) {
        Image(systemName: symbol).menuIcon(color)
        Text(title)
      }
    }
  }
}
