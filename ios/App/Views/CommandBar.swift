// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import CompassModel
import SwiftUI
import WebKit

/// Search or type an address; or jump to an open entry, a pinned tab or a
/// favorite. Everything it offers is the current space's profile's: its
/// spaces, history and other devices' tabs (M8), never another profile's.
///
/// The look: the space's tint behind everything, as in the sidebar; a large
/// search field on top with Cancel beside it; suggestions in rounded cards.
///
/// Ask in the command bar (docs/design/ai-sidebar.md): with "Answer
/// Questions in the Command Bar" on, a question gets an "Ask Claude" row on
/// top (Return asks); the answer streams into a card under the field, with
/// "Continue in Chat". Never in incognito.
///
/// Commands (Settings, Find on Page, Reader…) match by name, as the
/// desktop's: one the text names ("settings") leads, and Return runs it;
/// the others that match come last, under "Commands".
struct CommandBar: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var text = ""
  @FocusState private var focused: Bool
  /// Navigate the shown tab instead of opening a new one.
  let replacing: SidebarNode?
  /// Ask in the command bar: this bar's question and answer.
  @State private var asker = AppServices.shared.makeAsker()
  /// "About this page": the question goes with the shown tab's title and
  /// host.
  @State private var aboutPage = true

  var body: some View {
    VStack(spacing: 0) {
      header
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 4) {
          if asker.state != .idle {
            answerCard.padding(.bottom, 8)
          } else if offersAsk {
            askCard.padding(.bottom, 8)
          }
          let found = foundCommands
          if !found.named.isEmpty {
            Card {
              ForEach(Array(found.named.enumerated()), id: \.element.id) { index, command in
                commandRow(command, returnRuns: index == 0 && !offersAsk)
              }
            }
            .padding(.bottom, 8)
          }
          if !text.isEmpty {
            Card {
              Row(title: Self.short(text), subtitle: looksLikeURL
                    ? String(localized: "Open address", comment: "Command bar row subtitle: go to the typed address.")
                    : String(localized: "Search \(SearchEngine.current.name)", comment: "Command bar row subtitle: search with the search engine, e.g. Search Google."),
                  trailing: found.named.isEmpty ? "return" : "arrow.right", action: { go(text) }) {
                Image(systemName: looksLikeURL ? "globe" : "magnifyingglass")
                  .font(.system(size: 15, weight: .semibold))
                  .foregroundStyle(.white)
                  .frame(width: 32, height: 32)
                  .background(tint, in: RoundedRectangle(cornerRadius: 9))
              }
            }
            .padding(.bottom, 8)
          }
          // As Safari: the typed text, found on the shown page.
          if replacing != nil, !text.isEmpty, text != replacing?.url {
            CardSection(String(localized: "On this page", comment: "Command bar section: find the typed text on the shown page.")) {
              Row(title: String(localized: "Find “\(text)”", comment: "Command bar row: find the typed text on the page."), subtitle: Self.short(replacing?.url ?? ""), trailing: "arrow.right",
                  action: {
                    dismiss()
                    app.findOnPage(text)
                  }) {
                Image(systemName: "doc.text.magnifyingglass")
                  .font(.system(size: 15, weight: .semibold))
                  .frame(width: 32, height: 32)
                  .background(Color.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 9))
              }
            }
            .padding(.bottom, 4)
          }
          if !matches.isEmpty {
            CardSection(text.isEmpty ? String(localized: "Today", comment: "Command bar section: the space's Today tabs.") : String(localized: "Switch to tab", comment: "Command bar section: open tabs matching the text.")) {
              ForEach(matches) { node in
                Row(title: node.displayTitle, subtitle: spaceName(node), trailing: "arrow.right",
                    action: { app.show(node); dismiss() }) { SiteTile(url: node.url) }
              }
            }
          }
          let pages = app.sync.history.search(text, limit: 8, profile: account)
          if !pages.isEmpty {
            CardSection(text.isEmpty ? String(localized: "Recently visited", comment: "Command bar section: recent history.") : String(localized: "History", comment: "Command bar section: history matching the text.")) {
              ForEach(pages) { page in
                Row(title: page.title.isEmpty ? Self.short(page.url) : page.title,
                    subtitle: Self.short(page.url), action: { go(page.url) }) { SiteTile(url: page.url) }
              }
            }
          }
          ForEach(app.sync.foreignSessions(account: account)) { session in
            let tabs = session.tabs.filter {
              PhoneVisible.url($0.url)
            }.filter {
              text.isEmpty || $0.title.localizedCaseInsensitiveContains(text)
                || $0.url.localizedCaseInsensitiveContains(text)
            }
            if !tabs.isEmpty {
              CardSection(String(localized: "On \(session.name)", comment: "Command bar section: tabs open on another device, e.g. On Mac.")) {
                ForEach(tabs.prefix(text.isEmpty ? 5 : 20)) { tab in
                  Row(title: tab.title.isEmpty ? Self.short(tab.url) : tab.title,
                      subtitle: Self.short(tab.url), action: { app.open(tab.url); dismiss() }) {
                    SiteTile(url: tab.url)
                  }
                }
              }
            }
          }
          if !found.others.isEmpty {
            CardSection(String(localized: "Commands", comment: "Command bar section: commands matching the text.")) {
              ForEach(found.others) { commandRow($0, returnRuns: false) }
            }
          }
        }
        .padding(.horizontal, 14)
        .padding(.bottom, 24)
      }
      .scrollDismissesKeyboard(.interactively)
    }
    .background(SpaceTint(theme: app.selectedSpace?.theme ?? .blue))
    .spaceAppearance(app.selectedSpace?.theme)
    .presentationDragIndicator(.visible)
    // Where the sheet sits, for the pull's stand-in to end there.
    .onGeometryChange(for: CGFloat.self) { $0.frame(in: .global).minY } action: { top in
      if top > 0, replacing == nil { CommandBarPull.sheetTop = top }
    }
    // Another line than the one asked: its answer goes.
    .onChange(of: text) {
      if asker.state != .idle, text.trimmingCharacters(in: .whitespacesAndNewlines) != asker.question { asker.cancel() }
    }
    .onDisappear { asker.cancel() }
    .onAppear {
      text = replacing?.url ?? ""
      focused = true
      // Editing the shown tab's address: all of it selected, so typing
      // replaces it. Once the field has focus.
      if !text.isEmpty {
        Task {
          try? await Task.sleep(for: .milliseconds(150))
          UIApplication.shared.sendAction(#selector(UIResponder.selectAll(_:)), to: nil, from: nil, for: nil)
        }
      }
    }
  }

  /// "New tab in Personal" over the field and Cancel beside it.
  private var header: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(spacing: 6) {
        if let space = app.selectedSpace {
          SpaceIcon(space: space, size: 13).foregroundStyle(tint)
        }
        Text(replacing == nil ? "New tab in \(app.selectedSpace?.name ?? "Compass")" : "Go to")
          .font(.footnote.weight(.semibold))
          .foregroundStyle(.secondary)
      }
      .padding(.leading, 4)
      HStack(spacing: 12) {
        HStack(spacing: 8) {
          Image(systemName: "magnifyingglass")
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(.secondary)
          TextField("Search or enter address", text: $text)
            .font(.system(size: 17))
            .keyboardType(.webSearch)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .submitLabel(.go)
            .focused($focused)
            .onSubmit {
              if offersAsk {
                ask()
              } else if asker.state == .answered {
                // Return again: on in the chat, as the desktop's Enter.
                continueInChat()
              } else if let command = foundCommands.named.first {
                command.run()
              } else if !text.isEmpty {
                go(text)
              }
            }
          if !text.isEmpty {
            Button { text = "" } label: {
              Image(systemName: "xmark.circle.fill")
                .font(.system(size: 16))
                .foregroundStyle(.tertiary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Clear")
          }
        }
        .padding(.horizontal, 12)
        .frame(height: 46)
        .background(cardFill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(
          RoundedRectangle(cornerRadius: 14, style: .continuous)
            .strokeBorder(tint.opacity(focused ? 0.55 : 0), lineWidth: 1.5)
        )
        Button("Cancel") { dismiss() }
          .font(.body.weight(.medium))
          .foregroundStyle(.primary)
          .buttonStyle(.plain)
      }
    }
    .padding(.horizontal, 14)
    .padding(.top, 22)
    .padding(.bottom, 10)
  }

  // MARK: Ask in the command bar

  /// The shown web page (http or https), unless it's incognito: what
  /// "About this page" sends (its title and host only).
  private var page: (title: String, host: String)? {
    guard let tab = app.shownTab, !app.tabs.isIncognito(tab.id), let url = URL(string: tab.url),
      ["http", "https"].contains(url.scheme?.lowercased() ?? ""), let host = url.host(), !host.isEmpty
    else { return nil }
    let title = app.tabs.live[tab.id]?.title.flatMap { $0.isEmpty ? nil : $0 } ?? tab.title
    return (title, host)
  }

  /// Whether the "Ask Claude" row shows (and Return asks).
  private var offersAsk: Bool { asker.state == .idle && asker.offers(text, incognito: app.isIncognito) }

  private func ask() {
    asker.ask(text, page: aboutPage ? page : nil, incognito: app.isIncognito)
  }

  /// "Ask Claude", and "About this page" when a page is shown.
  private var askCard: some View {
    Card {
      Row(title: ClaudeSettings.shared.provider.askTitle, subtitle: Self.short(text), trailing: "return", action: ask) {
        Image(systemName: "sparkles")
          .font(.system(size: 15, weight: .semibold))
          .foregroundStyle(.white)
          .frame(width: 32, height: 32)
          .background(Color.orange, in: RoundedRectangle(cornerRadius: 9))
      }
      .accessibilityIdentifier("CommandBarAsk")
      if let page {
        Toggle(isOn: $aboutPage) {
          VStack(alignment: .leading, spacing: 1) {
            Text("About This Page").font(.subheadline)
            Text(page.host).font(.caption).foregroundStyle(.secondary).lineLimit(1)
          }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .accessibilityIdentifier("CommandBarAskAboutPage")
      }
    }
  }

  /// The answer as it streams, and what to do with it.
  private var answerCard: some View {
    Card {
      VStack(alignment: .leading, spacing: 10) {
        HStack(spacing: 8) {
          Image(systemName: "sparkles").foregroundStyle(.orange)
            .symbolEffect(.pulse, isActive: asker.state == .asking)
          Text(asker.question).font(.subheadline.weight(.semibold)).lineLimit(2)
        }
        switch asker.state {
        case .failed(let message):
          if !asker.answer.isEmpty { answerText }
          Label(message, systemImage: "exclamationmark.triangle.fill")
            .font(.footnote)
            .foregroundStyle(.red)
        case .asking where asker.answer.isEmpty:
          Text("Answering…").font(.callout).foregroundStyle(.secondary)
        default:
          answerText
        }
        if asker.state != .asking, !asker.answer.isEmpty {
          Button {
            continueInChat()
          } label: {
            Label("Continue in Chat", systemImage: "bubble.left.and.text.bubble.right")
              .font(.subheadline.weight(.medium))
          }
          .buttonStyle(.borderless)
          .accessibilityIdentifier("CommandBarContinueInChat")
        }
      }
      .padding(12)
      .frame(maxWidth: .infinity, alignment: .leading)
      .animation(.easeOut(duration: 0.2), value: asker.answer)
    }
  }

  private var answerText: some View {
    let markdown = try? AttributedString(
      markdown: asker.answer, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))
    return Text(markdown ?? AttributedString(asker.answer))
      .font(.callout)
      .textSelection(.enabled)
      .frame(maxWidth: .infinity, alignment: .leading)
  }

  /// The chat sheet, starting with the question and its answer.
  private func continueInChat() {
    let question = asker.message
    let answer = asker.answer
    dismiss()
    // After the bar has gone: a view shows one sheet at a time.
    Task {
      try? await Task.sleep(for: .milliseconds(450))
      app.claude.seed(question: question, answer: answer)
      app.showingClaude = true
    }
  }

  // MARK: Commands

  /// A command the bar offers.
  private struct Command: Identifiable {
    let title: String
    let subtitle: String
    let symbol: String
    let color: Color
    let run: () -> Void
    var id: String { title }
  }

  /// The commands matching the text (none while it's empty): those it
  /// names, which lead, and the others.
  private var foundCommands: (named: [Command], others: [Command]) {
    guard !text.trimmingCharacters(in: .whitespaces).isEmpty else { return ([], []) }
    let matching = commands.filter { CommandMatch.matches($0.title, text) }
    return (matching.filter { CommandMatch.names($0.title, text) },
            matching.filter { !CommandMatch.names($0.title, text) })
  }

  private func commandRow(_ command: Command, returnRuns: Bool) -> some View {
    Row(title: command.title, subtitle: command.subtitle, trailing: returnRuns ? "return" : nil,
        action: command.run) {
      Image(systemName: command.symbol).menuIcon(command.color)
    }
    .accessibilityIdentifier("CommandBarCommand")
  }

  /// What the bar can do now: with the shown tab's page, and the app's.
  private var commands: [Command] {
    var list: [Command] = []
    let tab = app.shownTab
    let view = tab.flatMap { app.tabs.live[$0.id] }
    let page = view?.url ?? tab.flatMap { URL(string: $0.url) }
    if tab != nil {
      list.append(Command(title: String(localized: "Find on Page", comment: "Command bar command."),
                          subtitle: String(localized: "Search the shown page", comment: "Command bar subtitle of the command Find on Page."),
                          symbol: "doc.text.magnifyingglass", color: .blue) {
        dismiss()
        app.findOnPage()
      })
    }
    if view != nil {
      list.append(Command(title: String(localized: "Reload", comment: "Command bar command."),
                          subtitle: String(localized: "Load the page again", comment: "Command bar subtitle of the command Reload."), symbol: "arrow.clockwise", color: .gray) {
        DevMode.shared.reload(view)
        dismiss()
      })
    }
    if let page {
      list.append(Command(title: String(localized: "Copy Link", comment: "Command bar command."), subtitle: Self.short(page.absoluteString), symbol: "link", color: .blue) {
        CopyLink.copy(page)
        dismiss()
      })
    }
    if app.shownReader != nil {
      list.append(Command(title: String(localized: "Leave Reader", comment: "Command bar command."),
                          subtitle: String(localized: "Back to the page", comment: "Command bar subtitle of the command Leave Reader."), symbol: "book", color: .brown) {
        app.toggleReader()
        dismiss()
      })
    } else if app.canShowReader {
      list.append(Command(title: String(localized: "Reader", comment: "Command bar command."),
                          subtitle: String(localized: "Read the article, without the rest", comment: "Command bar subtitle of the command Reader."), symbol: "book",
                          color: .brown) {
        app.toggleReader()
        dismiss()
      })
    }
    // As the menu's Pin tile: a top-level Today or Pinned entry, not in
    // incognito.
    if let tab, let space = app.store.space(of: tab), !space.isIncognito,
      tab.section == .today || tab.section == .pinned, tab.parent?.isSplit != true {
      let pinned = tab.section == .pinned
      list.append(Command(title: pinned ? String(localized: "Unpin Tab", comment: "Command bar command.") : String(localized: "Pin Tab", comment: "Command bar command."), subtitle: tab.displayTitle,
                          symbol: pinned ? "pin.slash" : "pin", color: .orange) {
        withAnimation(SidebarStyle.motion) {
          if pinned {
            _ = app.store.move(tab, to: space.today, at: space.today.children.count)
          } else {
            _ = app.store.move(tab, to: space.pinned, at: space.pinned.children.count)
          }
        }
        dismiss()
      })
    }
    if DevMode.shared.canToggle(page) {
      let on = DevMode.shared.isOn(page)
      list.append(Command(title: on ? String(localized: "Leave Development Mode", comment: "Command bar command.")
                            : String(localized: "Development Mode for This Site", comment: "Command bar command."),
                          subtitle: page?.host() ?? "", symbol: "chevron.left.forwardslash.chevron.right",
                          color: .gray) {
        DevMode.shared.toggle(page)
        dismiss()
      })
    }
    if let space = app.selectedSpace, app.tidier.canTidy(space) {
      list.append(Command(title: String(localized: "Tidy Today", comment: "Command bar command."),
                          subtitle: String(localized: "Group Today's entries into folders", comment: "Command bar subtitle of the command Tidy Today."), symbol: "sparkles",
                          color: .orange) {
        app.tidyToday(space)
        dismiss()
      })
    }
    if !app.isIncognito {
      list.append(Command(title: ClaudeSettings.shared.provider.chatTitle,
                          subtitle: String(localized: "Open the chat", comment: "Command bar subtitle of Chat with Claude."),
                          symbol: "bubble.left.and.text.bubble.right", color: .orange) {
        present { app.showingClaude = true }
      })
      list.append(Command(title: String(localized: "New Incognito Tab", comment: "Command bar command."),
                          subtitle: String(localized: "Nothing is remembered", comment: "Command bar subtitle of the command New Incognito Tab."), symbol: "eyeglasses",
                          color: .indigo) {
        present { app.openIncognitoTab() }
      })
    }
    if app.services.closed.canUndo {
      list.append(Command(title: String(localized: "Reopen Closed Tab", comment: "Command bar command."),
                          subtitle: String(localized: "Undo the last close", comment: "Command bar subtitle of the command Reopen Closed Tab."),
                          symbol: "arrow.uturn.backward", color: .blue) {
        app.undoClose()
        dismiss()
      })
    }
    list.append(Command(title: String(localized: "Downloads", comment: "Command bar command."),
                          subtitle: String(localized: "Files saved from pages", comment: "Command bar subtitle of the command Downloads."), symbol: "arrow.down.circle",
                        color: .blue) {
      present { app.showingDownloads = true }
    })
    list.append(Command(title: String(localized: "Settings", comment: "Command bar command."),
                          subtitle: String(localized: "Sync, passwords, search engine", comment: "Command bar subtitle of the command Settings."), symbol: "gearshape.fill",
                        color: .gray) {
      present { app.showingSettings = true }
    })
    return list
  }

  /// Closes the bar, then shows what `show` does: one sheet at a time.
  private func present(_ show: @escaping () -> Void) {
    dismiss()
    Task {
      try? await Task.sleep(for: .milliseconds(450))
      show()
    }
  }

  private func go(_ address: String) {
    app.open(address, replacing: replacing)
    dismiss()
  }

  private var tint: Color { app.selectedSpace?.theme.accent ?? .accentColor }

  private var looksLikeURL: Bool { AppModel.address(text) != nil }

  /// The profile account of the current space.
  private var account: String { app.selectedSpace.map(app.sync.accountProfile(of:)) ?? "" }

  private var matches: [SidebarNode] {
    let q = text.lowercased()
    let all = app.store.tabs(inProfileOf: app.selectedSpace).filter(\.isPhoneVisible)
    let found = q.isEmpty ? all.filter { $0.section == .today } : all.filter {
      $0.displayTitle.lowercased().contains(q) || $0.url.lowercased().contains(q)
    }
    return Array(found.prefix(12))
  }

  private func spaceName(_ node: SidebarNode) -> String {
    if node.section == .favorites { return String(localized: "Favorites", comment: "Command bar row subtitle: the tab is a favorite.") }
    let section = node.section == .pinned
      ? String(localized: "Pinned", comment: "Command bar row subtitle: the tab's sidebar section.")
      : String(localized: "Today", comment: "Command bar row subtitle: the tab's sidebar section.")
    return "\(app.store.space(of: node)?.name ?? "") · \(section)"
  }

  /// An address without its scheme, "www." or trailing slash.
  static func short(_ url: String) -> String {
    var s = url
    for prefix in ["https://", "http://", "www."] where s.hasPrefix(prefix) { s.removeFirst(prefix.count) }
    if s.hasSuffix("/") { s.removeLast() }
    return s
  }
}

/// The command bar for a new tab, pulled up from the bar's right end: a
/// sheet can't follow a finger, so this stand-in does, drawn as the sheet's
/// top (its handle, title, field and Cancel) with its top edge under the
/// finger. Let go high enough, it rises to where the sheet sits and the
/// real sheet replaces it (`AppModel.settleNewTabPull`).
struct CommandBarPull: View {
  @Environment(AppModel.self) private var app

  /// The sheet's corners and the page's dimming under it, as measured.
  static let radius: CGFloat = 34
  static let dim: CGFloat = 0.25
  /// The sheet's top on screen: measured each time it shows; before that,
  /// as measured on an iPhone, a little above the safe area's top.
  @MainActor static var sheetTop: CGFloat?
  @MainActor static var top: CGFloat { sheetTop ?? max(SidebarPanel.top - 8, 0) }

  /// The pull's 0...1 position with its top edge under the finger (at `y`
  /// on screen).
  @MainActor static func position(at y: CGFloat) -> CGFloat {
    let height = max(SidebarPanel.screenHeight - top, 1)
    return min(max((SidebarPanel.screenHeight - y) / height, 0), 1)
  }

  var body: some View {
    if let pull = app.newTabPull { panel(min(max(pull, 0), 1)) }
  }

  private func panel(_ pull: CGFloat) -> some View {
    let top = Self.top
    let height = SidebarPanel.screenHeight - top
    let theme = app.selectedSpace?.theme
    let tint = theme?.accent ?? .accentColor
    return ZStack(alignment: .top) {
      // The sheet dims the page itself once it's up.
      if !app.showingCommandBar { Color.black.opacity(Self.dim * pull) }
      VStack(alignment: .leading, spacing: 10) {
        HStack(spacing: 6) {
          if let space = app.selectedSpace { SpaceIcon(space: space, size: 13).foregroundStyle(tint) }
          Text("New tab in \(app.selectedSpace?.name ?? "Compass")")
            .font(.footnote.weight(.semibold))
            .foregroundStyle(.secondary)
        }
        .padding(.leading, 4)
        HStack(spacing: 12) {
          HStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
              .font(.system(size: 16, weight: .semibold))
              .foregroundStyle(.secondary)
            Text("Search or enter address")
              .font(.system(size: 17))
              .foregroundStyle(Color(.placeholderText))
            Spacer(minLength: 0)
          }
          .padding(.horizontal, 12)
          .frame(height: 46)
          .background(cardFill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
          .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(tint.opacity(0.55), lineWidth: 1.5))
          Text("Cancel").font(.body.weight(.medium))
        }
      }
      .padding(.horizontal, 14)
      .padding(.top, 22)
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
      .overlay(alignment: .top) {
        Capsule().fill(.secondary.opacity(0.5)).frame(width: 36, height: 5).padding(.top, 5)
      }
      .background(SpaceTint(theme: theme ?? .blue))
      .spaceAppearance(theme)
      .clipShape(UnevenRoundedRectangle(topLeadingRadius: Self.radius, topTrailingRadius: Self.radius, style: .continuous))
      .frame(height: height)
      .offset(y: top + (1 - pull) * height)
    }
    .ignoresSafeArea()
    .allowsHitTesting(false)
    .accessibilityHidden(true)
  }
}
