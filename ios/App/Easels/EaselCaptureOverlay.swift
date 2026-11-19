// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import SwiftUI
import WebKit

/// Capture to Easel (the page menu): a veil over the page; drag over the
/// region to capture, then pick the board (the last one used first, or a
/// new one). The region is taken from the web view (EaselCapture.capture).
struct EaselCaptureOverlay: View {
  @Environment(AppModel.self) private var app
  let tabID: String
  @State private var start: CGPoint?
  /// The region, in global (window) points.
  @State private var region: CGRect?
  @State private var origin: CGPoint = .zero
  @State private var choosing = false

  var body: some View {
    ZStack {
      veil
        .ignoresSafeArea()
        .onGeometryChange(for: CGPoint.self) { $0.frame(in: .global).origin } action: { origin = $0 }
        .gesture(
          DragGesture(minimumDistance: 2, coordinateSpace: .global)
            .onChanged { value in
              region = CGRect(corners: value.startLocation, value.location)
            }
            .onEnded { _ in
              if let r = region, r.width < 12 || r.height < 12 { region = nil }
            })
      VStack {
        Text(region == nil ? "Drag over what to capture" : "Capture this region?")
          .font(.subheadline.weight(.semibold))
          .padding(.horizontal, 16)
          .padding(.vertical, 10)
          .floatingGlass(in: Capsule(), frosted: true)
        Spacer()
        HStack(spacing: 12) {
          Button("Cancel") { app.easelCaptureTab = nil }
            .buttonStyle(.bordered)
          Button("Capture") { choosing = true }
            .buttonStyle(.borderedProminent)
            .disabled(region == nil)
        }
        .padding(.bottom, 90)
      }
      .padding(.top, 12)
    }
    .confirmationDialog("Add to Easel", isPresented: $choosing, titleVisibility: .visible) {
      ForEach(EaselCapture.boards.prefix(8), id: \.uuid) { board in
        Button(board.title.isEmpty ? "Untitled" : board.title) { capture(to: board.uuid) }
      }
      Button("New Easel") { captureToNew() }
      Button("Cancel", role: .cancel) {}
    }
  }

  /// Dimmed, with the region clear.
  private var veil: some View {
    Canvas { context, size in
      var path = Path(CGRect(origin: .zero, size: size))
      if let region {
        let local = region.offsetBy(dx: -origin.x, dy: -origin.y)
        path.addRect(local)
        context.fill(path, with: .color(.black.opacity(0.35)), style: FillStyle(eoFill: true))
        context.stroke(Path(local), with: .color(.white), style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
      } else {
        context.fill(path, with: .color(.black.opacity(0.35)))
      }
    }
    .contentShape(Rectangle())
  }

  private func captureToNew() {
    switch EaselStore.shared.createEasel(title: "Untitled Easel") {
    case .failure(let error): app.showMessage(error.message)
    case .success(let board):
      app.pinEasel(board)
      capture(to: board.uuid)
    }
  }

  private func capture(to board: String) {
    guard let region, let view = app.tabs.live[tabID] else {
      app.easelCaptureTab = nil
      return
    }
    // From window points to the web view's.
    let rect = view.convert(region, from: nil).intersection(view.bounds)
    app.easelCaptureTab = nil
    guard !rect.isNull, rect.width >= 4, rect.height >= 4 else { return }
    Task {
      switch await EaselCapture.capture(view, rect: rect, to: board) {
      case .success:
        let title = EaselStore.shared.easel(board)?.title ?? ""
        app.showMessage("Captured to “\(title.isEmpty ? "Untitled" : title)”")
      case .failure(let error): app.showMessage(error.message)
      }
    }
  }
}
