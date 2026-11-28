// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassTranslate
import SwiftUI

/// Settings › Translation (docs/design/translation.md, "iOS", "Engines"):
/// whether pages are offered, which engine, Compass's downloaded languages,
/// and the languages and sites never offered.
struct TranslationSettingsView: View {
  @AppStorage(TranslationPrefs.offerKey) private var offers = true
  @AppStorage(PageTranslation.engineKey) private var engine = "compass"
  @AppStorage(CompassTranslator.downloadsAllowedKey) private var downloadsAllowed = false
  @State private var installed: [String] = []
  @State private var never: [String] = []
  @State private var neverSites: [String] = []

  private let prefs = TranslationPrefs()

  var body: some View {
    Form {
      Section {
        Toggle("Offer to Translate Pages", isOn: $offers)
      } footer: {
        Text("A translate button shows in the address bar on pages in a language you don't read (your iPhone's preferred languages).")
      }
      if #available(iOS 18.0, *) {
        Section {
          Picker("Translate With", selection: $engine) {
            Text("Compass, Then Apple").tag("compass")
            Text("Apple, Then Compass").tag("apple")
          }
        } footer: {
          Text("Compass's own models translate most languages; Apple's translate the ones Compass doesn't, or go first if you choose. Apple's models are shared with Safari and managed in the Settings app. Both stay on this device.")
        }
      }
      Section {
        Toggle("Download Languages When Needed", isOn: $downloadsAllowed)
        ForEach(installed, id: \.self) { language in
          HStack {
            Text(PageTranslation.name(language))
            Spacer()
            Button("Remove", role: .destructive) {
              CompassTranslator.shared.uninstall(language)
              reload()
            }
            .buttonStyle(.borderless)
          }
        }
      } header: {
        Text("Compass's Languages")
      } footer: {
        Text("Firefox's open translation models, downloaded from Compass's server once per language (about 45 MB each). The server sees your IP address and which language, never the page.")
      }
      if !never.isEmpty {
        Section("Never Translate") {
          ForEach(never, id: \.self) { Text(PageTranslation.name($0)) }
            .onDelete { offsets in
              prefs.never.subtract(offsets.map { never[$0] })
              reload()
            }
        }
      }
      if !neverSites.isEmpty {
        Section("Never Translate These Sites") {
          ForEach(neverSites, id: \.self) { Text($0) }
            .onDelete { offsets in
              prefs.neverSites.subtract(offsets.map { neverSites[$0] })
              reload()
            }
        }
      }
    }
    .navigationTitle("Translation")
    .onAppear(perform: reload)
  }

  private func reload() {
    installed = CompassTranslator.shared.packs?.installed ?? []
    never = prefs.never.sorted { PageTranslation.name($0) < PageTranslation.name($1) }
    neverSites = prefs.neverSites.sorted()
  }
}
