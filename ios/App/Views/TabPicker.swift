// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

/// The tab picker: the loaded tabs as cards in a row, least recently shown
/// at the left, over the space's tint. Cards left of the centered one stack
/// behind it, peeking out; cards to its right lie on top, side by side, so a
/// swipe right pulls the stack apart. A tap shows a card's tab, a flick up
/// closes it, a drag down closes the picker onto the centered card.
///
/// It shows the regular spaces' tabs or incognito's, never both: incognito's
/// when the window is in incognito, and a toggle by + switches. While
/// incognito is locked or covered, its side shows the lock instead of cards.
///
/// It opens from a swipe up on the bar's left end and follows the finger:
/// at 0 the centered card fills the screen exactly where the page is (the
/// whole row is scaled up about it), at 1 it rests as a card. Closing runs
/// the same way back, onto the page now shown.
struct TabPicker: View {
  @Environment(AppModel.self) private var app
  /// A drag on a card or the background: its axis, nil until it picks one.
  @State private var axis: Axis?
  @State private var dragStart: CGFloat = 0
  /// A card being flicked up: its ID and how far it's gone.
  @State private var flicked: String?
  @State private var flickY: CGFloat = 0

  /// A card's size, as a fraction of the screen.
  static let scale: CGFloat = 0.7
  /// The card slot's center, as a fraction of the screen's height.
  static let centerY: CGFloat = 0.5
  static let gap: CGFloat = 14
  /// A finger travels this far to open or close the picker.
  static let travel: CGFloat = 300

  var body: some View {
    let progress = app.pickerProgress
    // Not there at all while closed: not rendered, not in VoiceOver
    // (accessibilityHidden doesn't take an overlay out of it).
    if progress > 0 || app.pickerHolding { picker(progress) }
  }

  private func picker(_ progress: CGFloat) -> some View {
    GeometryReader { geo in
      let size = geo.size
      ZStack {
        SpaceTint(theme: app.pickerSpace?.theme ?? .blue)
          .overlay(Color.black.opacity(0.18))
          // Closing onto another tab, the old page mustn't show around the
          // growing card.
          .opacity(app.pickerHolding ? max(progress, switching ? 1 : 0) : progress)
          .contentShape(Rectangle())
          .gesture(drag(id: nil, size: size))
        if locked {
          lockedNote.opacity(progress)
        } else {
          if app.pickerTabs.isEmpty {
            Text(app.pickerIncognito ? "No incognito tabs" : "No open tabs")
              .font(.headline).foregroundStyle(.secondary).opacity(progress)
          }
          row(size: size, progress: progress)
        }
        bottomBar.opacity(progress)
      }
      .frame(width: size.width, height: size.height)
    }
    .ignoresSafeArea()
    .spaceAppearance(app.pickerSpace?.theme)
    .allowsHitTesting(app.showingPicker)
  }

  /// Incognito's side while it's locked or covered: no cards (nor their
  /// snapshots), the lock.
  private var locked: Bool { app.pickerIncognito && app.incognitoHidden }

  private var lockedNote: some View {
    VStack(spacing: 14) {
      Image(systemName: "eyeglasses").font(.system(size: 44, weight: .semibold))
      if app.services.incognitoLocked {
        Text("Incognito is locked").font(.headline)
        Button("Unlock") { Task { await app.services.unlockIncognito() } }
          .buttonStyle(.borderedProminent)
          .tint(.white.opacity(0.25))
          .accessibilityIdentifier("PickerUnlockIncognito")
      }
    }
    .foregroundStyle(.white)
  }

  /// At the bottom: the toggle, and + (not while incognito's is locked).
  private var bottomBar: some View {
    VStack {
      Spacer()
      HStack(spacing: 12) {
        modeToggle
        if !locked { plusButton }
      }
      .padding(.bottom, SidebarPanel.bottomInset + 8)
    }
  }

  /// Regular tabs or incognito's.
  private var modeToggle: some View {
    HStack(spacing: 0) {
      modeButton("Tabs", symbol: "square.on.square", incognito: false)
      modeButton("Incognito", symbol: "eyeglasses", incognito: true)
    }
    .padding(4)
    .floatingGlass(in: Capsule())
  }

  private func modeButton(_ title: String, symbol: String, incognito: Bool) -> some View {
    let on = app.pickerIncognito == incognito
    return Button {
      withAnimation(.snappy(duration: 0.3)) { app.setPickerIncognito(incognito) }
    } label: {
      Label(title, systemImage: symbol)
        .font(.system(size: 15, weight: .semibold))
        .padding(.horizontal, 14)
        .frame(height: 36)
        .background(Capsule().fill(on ? Color.primary.opacity(0.15) : .clear))
        .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .foregroundStyle(.primary)
    .accessibilityAddTraits(on ? .isSelected : [])
    .accessibilityIdentifier(incognito ? "PickerIncognito" : "PickerRegular")
  }

  /// The centered card isn't the page shown: closing switches tabs.
  private var switching: Bool {
    let index = Int(app.pickerPosition.rounded())
    guard app.pickerTabs.indices.contains(index) else { return false }
    return app.pickerTabs[index] != app.shownTab?.id
  }

  /// The cards, laid out at rest, then scaled about the centered slot for
  /// the opening.
  private func row(size: CGSize, progress: CGFloat) -> some View {
    let s = Self.scale
    let zoom = 1 / s + (1 - 1 / s) * progress
    return ZStack {
      ForEach(Array(app.pickerTabs.enumerated()), id: \.element) { index, id in
        if let node = app.store.node(id: id) {
          let d = CGFloat(index) - app.pickerPosition
          card(node, index: index, d: d, size: size, progress: progress)
            .zIndex(Double(index))
        }
      }
    }
    .frame(width: size.width, height: size.height)
    .scaleEffect(zoom, anchor: UnitPoint(x: 0.5, y: Self.centerY))
    // The slot's center isn't the screen's: at 0 the card sits on the page.
    .offset(y: (0.5 - Self.centerY) * size.height * (1 - progress))
  }

  private func card(_ node: SidebarNode, index: Int, d: CGFloat, size: CGSize, progress: CGFloat) -> some View {
    let s = Self.scale
    let width = size.width * s
    let height = size.height * s
    // The page's corners at 0 (the screen's, shrunk with the card), a
    // card's at 1.
    let radius = 55 * s + (26 - 55 * s) * progress
    let stacked = min(max(-d, 0), 3)
    let x: CGFloat =
      d >= 0
      ? d * (width + Self.gap)
      : -(min(stacked, 1) * size.width * 0.13 + max(stacked - 1, 0) * size.width * 0.025)
    let flicking = flicked == node.id
    return VStack(alignment: .leading, spacing: 10) {
      HStack(spacing: 10) {
        SiteIcon(url: node.url, size: 26)
        Text(node.displayTitle)
          .font(.system(size: 17, weight: .medium))
          .lineLimit(1)
          .opacity(max(0, 1 - abs(d) * 2))
      }
      .frame(width: width, alignment: .leading)
      .opacity(progress)
      ZStack {
        SpaceTint(theme: app.store.space(of: node)?.theme ?? .blue)
        if let image = app.tabs.snapshots[node.id] {
          Image(uiImage: image).resizable().aspectRatio(contentMode: .fill)
            .frame(width: width, height: height, alignment: .top)
        } else {
          SiteIcon(url: node.url, size: 56)
        }
        // Stacked cards darken as they go back.
        Color.black.opacity(0.22 * min(stacked, 1) * progress)
      }
      .frame(width: width, height: height)
      .overlay(alignment: .bottomLeading) {
        badge(for: node).padding(10).opacity(progress)
      }
      .clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
      // Only near the middle: a dozen live shadows cost frames.
      .shadow(color: .black.opacity(abs(d) < 1.5 ? 0.3 * progress : 0), radius: 16, y: 6)
    }
    .scaleEffect(1 - 0.05 * stacked)
    // On the slot (the ZStack centers it): the title row sits above the
    // card, so the card itself is centered there.
    .offset(x: x, y: (Self.centerY - 0.5) * size.height - (26 + 10) / 2 + (flicking ? flickY : 0))
    .opacity(abs(d) < 0.5 ? 1 : progress)
    .opacity(flicking ? 1 - min(-flickY / size.height, 1) : 1)
    .onTapGesture { app.pickTab(at: index) }
    .gesture(drag(id: node.id, size: size))
    .accessibilityElement(children: .ignore)
    .accessibilityAddTraits(.isButton)
    .accessibilityLabel(node.displayTitle)
    .accessibilityValue(place(of: node))
    .accessibilityIdentifier("PickerCard")
    .accessibilityAction(named: "Close") { app.closeFromPicker(node.id) }
  }

  /// Where the card's tab lives, at its bottom left corner (still showing on
  /// stacked cards): its space's icon and name on its accent, or Favorites
  /// (a profile's, shown with every space of it, so no space's).
  @ViewBuilder private func badge(for node: SidebarNode) -> some View {
    if let space = app.store.space(of: node) {
      badge(name: space.name, color: space.theme.accent) { SpaceIcon(space: space, size: 13) }
    } else {
      badge(name: "Favorites", color: .gray) {
        Image(systemName: "star.fill").font(.system(size: 11.5, weight: .semibold))
      }
    }
  }

  private func badge(name: String, color: Color, @ViewBuilder icon: () -> some View) -> some View {
    HStack(spacing: 5) {
      icon()
      Text(name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
    }
    .foregroundStyle(.white)
    .padding(.horizontal, 9)
    .padding(.vertical, 5)
    .background(Capsule().fill(color))
    .shadow(color: .black.opacity(0.2), radius: 3, y: 1)
    .accessibilityHidden(true)
  }

  /// The badge's words for VoiceOver.
  private func place(of node: SidebarNode) -> String {
    app.store.space(of: node)?.name ?? "Favorites"
  }

  /// Sideways: scrolls the row. Up on a card: flicks it away. Down: closes
  /// the picker, following the finger.
  private func drag(id: String?, size: CGSize) -> some Gesture {
    let spacing = size.width * Self.scale + Self.gap
    let last = CGFloat(max(app.pickerTabs.count - 1, 0))
    return DragGesture(minimumDistance: 10, coordinateSpace: .global)
      .onChanged { value in
        let (dx, dy) = (value.translation.width, value.translation.height)
        if axis == nil {
          axis = abs(dx) > abs(dy) ? .horizontal : .vertical
          dragStart = app.pickerPosition
        }
        if axis == .horizontal {
          var p = dragStart - dx / spacing
          // Past either end: a rubber band.
          if p < 0 { p /= 3 } else if p > last { p = last + (p - last) / 3 }
          app.pickerPosition = p
        } else if dy < 0, let id {
          flicked = id
          flickY = dy
        } else if dy > 0 {
          flicked = nil
          app.pickerDrag = max(0, 1 - dy / Self.travel)
        }
      }
      .onEnded { value in
        defer { axis = nil }
        if axis == .horizontal {
          let target = (dragStart - value.predictedEndTranslation.width / spacing).rounded()
          // At most a few cards per flick.
          let clamped = min(max(target, dragStart.rounded() - 3, 0), dragStart.rounded() + 3, last)
          withAnimation(.snappy(duration: 0.35)) { app.pickerPosition = clamped }
        } else if let flickedID = flicked {
          let gone = value.translation.height < -size.height * 0.2
            || value.predictedEndTranslation.height < -size.height * 0.35
          if gone {
            withAnimation(.easeOut(duration: 0.18)) { flickY = -size.height }
            Task {
              try? await Task.sleep(for: .milliseconds(180))
              withAnimation(.snappy(duration: 0.3)) { app.closeFromPicker(flickedID) }
              flicked = nil
              flickY = 0
            }
          } else {
            withAnimation(.snappy(duration: 0.3)) { flickY = 0 }
            flicked = nil
          }
        } else if app.pickerDrag != nil {
          app.settlePicker(predicted: 1 - value.predictedEndTranslation.height / Self.travel)
        }
      }
  }

  /// A new tab: the picker closes onto the centered card, the command bar
  /// opens over it.
  private var plusButton: some View {
    Button {
      app.newTabFromPicker()
    } label: {
      Image(systemName: "plus")
        .font(.system(size: 22, weight: .semibold))
        .frame(width: 84, height: 48)
        .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .foregroundStyle(.primary)
    .floatingGlass(in: Capsule())
    .accessibilityLabel(app.pickerIncognito ? "New incognito tab" : "New tab")
    .accessibilityIdentifier("PickerNewTab")
  }
}

extension AppModel {
  /// How far open the picker is: under a finger, else 0 or 1.
  var pickerProgress: CGFloat {
    if let drag = pickerDrag { return min(max(drag, 0), 1) }
    return showingPicker ? 1 : 0
  }
}
