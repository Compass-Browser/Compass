// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
      }
      .navigationTitle(site == nil ? "Global Site Settings" : "Site Settings")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        if site != nil { Button("Done") { dismiss() } }
      }
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
