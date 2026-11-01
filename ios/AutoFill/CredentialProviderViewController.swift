// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AuthenticationServices
import CompassSync
import SwiftUI

/// Compass passwords in Safari and other apps: the QuickType bar (the app
/// keeps the system's identity list current) and the password list.
final class CredentialProviderViewController: ASCredentialProviderViewController {
  private lazy var store: any PasswordStore = {
    let group = Bundle.main.object(forInfoDictionaryKey: "CompassKeychainGroup") as? String
    if let group, !group.hasPrefix("$"), !group.hasPrefix(".") { return KeychainPasswordStore(accessGroup: group) }
    return KeychainPasswordStore(accessGroup: nil)
  }()

  override func prepareCredentialList(for serviceIdentifiers: [ASCredentialServiceIdentifier]) {
    let all = store.all().filter { !$0.isBlocked && !$0.password.isEmpty }
    let hosts = serviceIdentifiers.compactMap { id -> String? in
      id.type == .URL ? URL(string: id.identifier)?.host() : id.identifier
    }
    let suggested = all.filter { p in hosts.contains { $0 == p.host || $0.hasSuffix("." + p.host) || p.host.hasSuffix("." + $0) } }
    let list = PasswordList(
      suggested: suggested, all: all.sorted { $0.host < $1.host },
      pick: { [weak self] p in self?.complete(p) },
      cancel: { [weak self] in
        self?.extensionContext.cancelRequest(withError: NSError(domain: ASExtensionErrorDomain, code: ASExtensionError.userCanceled.rawValue))
      })
    let host = UIHostingController(rootView: list)
    addChild(host)
    host.view.frame = view.bounds
    host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    view.addSubview(host.view)
    host.didMove(toParent: self)
  }

  /// QuickType: fill without UI when the record is there.
  override func provideCredentialWithoutUserInteraction(for request: any ASCredentialRequest) {
    guard let identity = request.credentialIdentity as? ASPasswordCredentialIdentity,
      let p = store.all().first(where: { $0.tag == identity.recordIdentifier })
    else {
      extensionContext.cancelRequest(withError: NSError(domain: ASExtensionErrorDomain, code: ASExtensionError.credentialIdentityNotFound.rawValue))
      return
    }
    complete(p)
  }

  private func complete(_ p: SavedPassword) {
    extensionContext.completeRequest(withSelectedCredential: ASPasswordCredential(user: p.username, password: p.password))
  }
}

struct PasswordList: View {
  let suggested: [SavedPassword]
  let all: [SavedPassword]
  let pick: (SavedPassword) -> Void
  let cancel: () -> Void
  @State private var query = ""

  var body: some View {
    NavigationStack {
      List {
        if !suggested.isEmpty && query.isEmpty {
          Section("Suggested") { ForEach(suggested) { row($0) } }
        }
        Section("All passwords") {
          ForEach(all.filter { query.isEmpty || $0.host.localizedCaseInsensitiveContains(query) || $0.username.localizedCaseInsensitiveContains(query) }) { row($0) }
        }
      }
      .searchable(text: $query)
      .navigationTitle("Compass")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar { Button("Cancel", action: cancel) }
    }
  }

  private func row(_ p: SavedPassword) -> some View {
    Button { pick(p) } label: {
      VStack(alignment: .leading) {
        Text(p.host).foregroundStyle(.primary)
        Text(p.username).font(.caption).foregroundStyle(.secondary)
      }
    }
  }
}
