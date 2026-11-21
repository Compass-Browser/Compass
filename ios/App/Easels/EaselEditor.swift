// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import CompassModel
import PhotosUI
import SwiftUI

/// The board editor (docs/design/easels.md, "Editor"), native in place of
/// the desktop's WebUI page: pan with a drag on empty space, zoom with a
/// pinch; select, move and resize; text, rectangle, circle, arrow and pen;
/// images from Photos or the clipboard; a URL pasted becomes a link card.
/// Every edit saves at once (a drag when the finger lifts); Undo and Redo
/// are the editor's own history, not synced.
struct EaselEditor: View {
  @Environment(AppModel.self) private var app
  let board: String
  let tab: SidebarNode
  var topInset: CGFloat = 0
  var bottomInset: CGFloat = 0

  enum Tool: String, CaseIterable {
    case select, area, text, rectangle, circle, arrow, pen

    var symbol: String {
      switch self {
      case .select: "hand.point.up.left"
      case .area: "rectangle.dashed"
      case .text: "textformat"
      case .rectangle: "rectangle"
      case .circle: "circle"
      case .arrow: "arrow.up.right"
      case .pen: "scribble"
      }
    }

    var title: String {
      switch self {
      case .select: "Select"
      case .area: "Select Area"
      case .text: "Text"
      case .rectangle: "Rectangle"
      case .circle: "Circle"
      case .arrow: "Arrow"
      case .pen: "Pen"
      }
    }

    /// A drag draws rather than pans.
    var draws: Bool { self != .select && self != .text }
  }

  /// Screen = board × zoom + offset.
  @State private var zoom: CGFloat = 1
  @State private var offset = CGPoint(x: 24, y: 24)
  @State private var viewSize: CGSize = .zero
  @State private var placed = false
  @State private var tool: Tool = .select
  @State private var color: EaselColor = .ink
  @State private var selection: Set<String> = []
  /// While selected objects are dragged: how far, on screen.
  @State private var moveBy: CGSize = .zero
  /// While the selected object's corner is dragged: how far, on screen.
  @State private var resizeBy: CGSize = .zero
  @State private var panStart: CGPoint?
  @State private var pinchStart: Pinch?
  /// What's being drawn, in board points: a shape's two corners, or a
  /// stroke's points.
  @State private var draft: [CGPoint] = []
  /// The text being edited (a new one isn't saved until it has text).
  @State private var editing: EaselObject?
  @State private var editingIsNew = false
  @State private var editText = ""
  @FocusState private var editFocused: Bool
  @State private var title = ""
  @State private var history = EaselHistory()
  @State private var photo: PhotosPickerItem?
  @State private var refreshing: Set<String> = []

  struct Pinch {
    var zoom: CGFloat
    var offset: CGPoint
  }

  private var store: EaselStore { EaselStore.shared }
  private var easel: EaselBoard? { store.easel(board) }
  private var palette: EaselPalette { EaselPalette(easel?.palette ?? "") }

  var body: some View {
    let objects = store.objects(of: board)
    ZStack(alignment: .topLeading) {
      background
      ForEach(objects, id: \.uuid) { object in
        if editing?.uuid != object.uuid { objectView(object) }
      }
      selectionLayer(objects)
      draftLayer
      if let editing { textEditor(editing) }
    }
    .clipped()
    .onGeometryChange(for: CGSize.self) { $0.size } action: { size in
      viewSize = size
      if !placed, size.width > 0 {
        placed = true
        fit(objects, animated: false)
      }
    }
    .overlay(alignment: .top) { topBar.padding(.top, topInset + 6) }
    .overlay(alignment: .bottom) { bottomBars(objects).padding(.bottom, bottomInset + 8) }
    .ignoresSafeArea(.keyboard)
    .onAppear {
      title = easel?.title ?? ""
      // The tab follows the board's title.
      if let easel, tab.title != easel.title { app.store.update(tab, title: easel.title) }
    }
    .onChange(of: easel?.title) { _, new in
      if !editTitleFocused, let new { title = new }
    }
    .onChange(of: photo) { _, item in
      guard let item else { return }
      photo = nil
      Task {
        guard let data = try? await item.loadTransferable(type: Data.self) else { return }
        await addImage(data)
      }
    }
    .onDisappear { finishEditing() }
  }

  @FocusState private var editTitleFocused: Bool

  // MARK: Coordinates

  private func toScreen(_ p: CGPoint) -> CGPoint { CGPoint(x: p.x * zoom + offset.x, y: p.y * zoom + offset.y) }
  private func toBoard(_ p: CGPoint) -> CGPoint { CGPoint(x: (p.x - offset.x) / zoom, y: (p.y - offset.y) / zoom) }

  private func screenFrame(_ o: EaselObject) -> CGRect {
    var frame = CGRect(x: o.x * zoom + offset.x, y: o.y * zoom + offset.y, width: o.width * zoom, height: o.height * zoom)
    if selection.contains(o.uuid) {
      frame = frame.offsetBy(dx: moveBy.width, dy: moveBy.height)
      if selection.count == 1 {
        frame.size.width = max(frame.width + resizeBy.width, 8 * zoom)
        frame.size.height = max(frame.height + resizeBy.height, 8 * zoom)
      }
    }
    return frame
  }

  /// The board point at the middle of what's visible.
  private var visibleCenter: CGPoint {
    toBoard(CGPoint(x: viewSize.width / 2, y: (viewSize.height + topInset - bottomInset) / 2))
  }

  private func rememberView() { EaselViewMemory.centers[board] = visibleCenter }

  // MARK: Background, panning and drawing

  private var background: some View {
    Canvas { context, size in
      context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(palette.background))
      let step = 32 * zoom
      guard step >= 8, palette == .grid || palette == .dots || palette == .dark else { return }
      let startX = offset.x.truncatingRemainder(dividingBy: step) - step
      let startY = offset.y.truncatingRemainder(dividingBy: step) - step
      if palette == .dots {
        let r = max(1, 1.2 * zoom)
        var y = startY
        while y < size.height + step {
          var x = startX
          while x < size.width + step {
            context.fill(Path(ellipseIn: CGRect(x: x - r, y: y - r, width: r * 2, height: r * 2)), with: .color(palette.lines.opacity(2)))
            x += step
          }
          y += step
        }
      } else {
        var path = Path()
        var x = startX
        while x < size.width + step { path.move(to: CGPoint(x: x, y: 0)); path.addLine(to: CGPoint(x: x, y: size.height)); x += step }
        var y = startY
        while y < size.height + step { path.move(to: CGPoint(x: 0, y: y)); path.addLine(to: CGPoint(x: size.width, y: y)); y += step }
        context.stroke(path, with: .color(palette.lines), lineWidth: 1)
      }
    }
    .contentShape(Rectangle())
    .onTapGesture(coordinateSpace: .local) { location in backgroundTap(location) }
    .gesture(backgroundDrag)
    .simultaneousGesture(magnify)
    .accessibilityLabel("Board")
  }

  private var backgroundDrag: some Gesture {
    DragGesture(minimumDistance: tool.draws ? 0 : 6, coordinateSpace: .local)
      .onChanged { value in
        guard pinchStart == nil else { return }
        switch tool {
        case .select, .text:
          if panStart == nil {
            panStart = offset
            finishEditing()
          }
          if let start = panStart {
            offset = CGPoint(x: start.x + value.translation.width, y: start.y + value.translation.height)
          }
        case .pen:
          if draft.isEmpty { draft = [toBoard(value.startLocation)] }
          draft.append(toBoard(value.location))
        case .area, .rectangle, .circle, .arrow:
          draft = [toBoard(value.startLocation), toBoard(value.location)]
        }
      }
      .onEnded { _ in
        if panStart != nil {
          panStart = nil
          rememberView()
        }
        if !draft.isEmpty {
          finishDraft()
          draft = []
        }
      }
  }

  private var magnify: some Gesture {
    MagnifyGesture()
      .onChanged { value in
        if pinchStart == nil {
          pinchStart = Pinch(zoom: zoom, offset: offset)
          panStart = nil
          draft = []
        }
        guard let start = pinchStart else { return }
        let newZoom = min(max(start.zoom * value.magnification, 0.1), 6)
        let anchor = value.startLocation
        let ratio = newZoom / start.zoom
        zoom = newZoom
        offset = CGPoint(x: anchor.x - (anchor.x - start.offset.x) * ratio, y: anchor.y - (anchor.y - start.offset.y) * ratio)
      }
      .onEnded { _ in
        pinchStart = nil
        rememberView()
      }
  }

  private func backgroundTap(_ location: CGPoint) {
    if editing != nil {
      finishEditing()
      return
    }
    if tool == .text {
      var o = EaselObject()
      o.easelUuid = board
      let p = toBoard(location)
      o.x = p.x
      o.y = p.y - 12
      o.width = 240
      o.height = 28
      o.text.text = ""
      if let css = color.css { o.text.color = css }
      beginEditing(o, isNew: true)
      tool = .select
      return
    }
    selection = []
  }

  private var draftLayer: some View {
    Canvas { context, _ in
      guard draft.count >= 2 else { return }
      let ink = color.color(on: palette)
      let style = StrokeStyle(lineWidth: 3 * zoom, lineCap: .round, lineJoin: .round)
      let points = draft.map(toScreen)
      switch tool {
      case .pen:
        var path = Path()
        path.addLines(points)
        context.stroke(path, with: .color(ink), style: style)
      case .rectangle, .area:
        let rect = CGRect(corners: points[0], points[1])
        if tool == .area {
          context.fill(Path(rect), with: .color(.accentColor.opacity(0.1)))
          context.stroke(Path(rect), with: .color(.accentColor), style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
        } else {
          context.stroke(Path(rect), with: .color(ink), style: style)
        }
      case .circle:
        context.stroke(Path(ellipseIn: CGRect(corners: points[0], points[1])), with: .color(ink), style: style)
      case .arrow:
        var path = Path()
        path.addLines([points[0], points[1]])
        context.stroke(path, with: .color(ink), style: style)
      default: break
      }
    }
    .allowsHitTesting(false)
  }

  /// A drawn shape or stroke becomes an object; an area selects what's in it.
  private func finishDraft() {
    let points = draft
    guard let first = points.first, let last = points.last else { return }
    if tool == .area {
      let area = CGRect(corners: first, last)
      selection = Set(store.objects(of: board).filter { area.intersects(CGRect(x: $0.x, y: $0.y, width: $0.width, height: $0.height)) }.map(\.uuid))
      tool = .select
      return
    }
    var o = EaselObject()
    o.easelUuid = board
    var shape = SyncPb_CompassEaselShape()
    if let css = color.css { shape.color = css }
    let pad: Double = 8
    switch tool {
    case .rectangle, .circle:
      var rect = CGRect(corners: first, last)
      // A tap draws one of a default size.
      if rect.width < 6 && rect.height < 6 { rect = CGRect(x: first.x, y: first.y, width: 160, height: 100) }
      shape.kind = (tool == .rectangle ? EaselShapeKind.rectangle : .circle).rawValue
      o.x = rect.minX
      o.y = rect.minY
      o.width = max(rect.width, 4)
      o.height = max(rect.height, 4)
    case .arrow:
      var end = last
      if hypot(last.x - first.x, last.y - first.y) < 6 { end = CGPoint(x: first.x + 140, y: first.y) }
      let rect = CGRect(corners: first, end).insetBy(dx: -pad, dy: -pad)
      shape.kind = EaselShapeKind.arrow.rawValue
      shape.points = [first.x - rect.minX, first.y - rect.minY, end.x - rect.minX, end.y - rect.minY]
      o.x = rect.minX
      o.y = rect.minY
      o.width = rect.width
      o.height = rect.height
    case .pen:
      let xs = points.map(\.x), ys = points.map(\.y)
      let rect = CGRect(x: xs.min()!, y: ys.min()!, width: xs.max()! - xs.min()!, height: ys.max()! - ys.min()!)
        .insetBy(dx: -pad, dy: -pad)
      shape.kind = EaselShapeKind.freehand.rawValue
      shape.points = points.flatMap { [$0.x - rect.minX, $0.y - rect.minY] }
      o.x = rect.minX
      o.y = rect.minY
      o.width = rect.width
      o.height = rect.height
    default: return
    }
    o.shape = shape
    o.z = topZ + 1
    if let saved = apply([(nil, o)]).first ?? nil, tool != .pen {
      selection = [saved.uuid]
      tool = .select
    }
  }

  private var topZ: Double { store.objects(of: board).last?.z ?? 0 }
  private var bottomZ: Double { store.objects(of: board).first?.z ?? 0 }

  // MARK: Objects

  private func objectView(_ o: EaselObject) -> some View {
    let frame = screenFrame(o)
    let resizing = selection == [o.uuid] && resizeBy != .zero
    return EaselObjectView(
      object: o, palette: palette, zoom: zoom,
      size: resizing ? CGSize(width: frame.width / zoom, height: frame.height / zoom) : nil
    )
    .opacity(refreshing.contains(o.uuid) ? 0.5 : 1)
    .overlay { if refreshing.contains(o.uuid) { ProgressView() } }
    .contentShape(Rectangle())
    .onTapGesture(count: 2) { if o.hasText { beginEditing(o, isNew: false) } }
    .onTapGesture { tapped(o) }
    .gesture(moveGesture(o))
    .contextMenu { objectMenu(o) }
    .allowsHitTesting(tool == .select || tool == .text)
    .position(x: frame.midX, y: frame.midY)
    .accessibilityLabel(Self.describe(o))
  }

  static func describe(_ o: EaselObject) -> String {
    if o.hasText { return o.text.text }
    if o.hasShape { return "Drawing" }
    if o.hasImage { return "Image" }
    if o.hasLink { return "Link: \(o.link.title.isEmpty ? o.link.url : o.link.title)" }
    if o.hasCapture { return "Capture of \(o.capture.title.isEmpty ? o.capture.url : o.capture.title)" }
    return "Unknown item"
  }

  /// A link or capture opens its page in a new tab; anything else is
  /// selected.
  private func tapped(_ o: EaselObject) {
    if editing != nil { finishEditing() }
    let url = o.hasLink ? o.link.url : o.hasCapture ? o.capture.url : ""
    if !url.isEmpty, !selection.contains(o.uuid), let u = URL(string: url) {
      app.openFromEasel(u)
      return
    }
    selection = [o.uuid]
  }

  private func moveGesture(_ o: EaselObject) -> some Gesture {
    DragGesture(minimumDistance: 4, coordinateSpace: .local)
      .onChanged { value in
        if !selection.contains(o.uuid) { selection = [o.uuid] }
        moveBy = value.translation
      }
      .onEnded { value in
        let dx = value.translation.width / zoom, dy = value.translation.height / zoom
        let moved = store.objects(of: board).filter { selection.contains($0.uuid) }
        apply(moved.map { before in
          var after = before
          after.x += dx
          after.y += dy
          return (before, after)
        })
        moveBy = .zero
      }
  }

  @ViewBuilder private func selectionLayer(_ objects: [EaselObject]) -> some View {
    let selected = objects.filter { selection.contains($0.uuid) }
    ForEach(selected, id: \.uuid) { o in
      let frame = screenFrame(o)
      RoundedRectangle(cornerRadius: 3)
        .stroke(Color.accentColor, lineWidth: 1.5)
        .frame(width: frame.width + 6, height: frame.height + 6)
        .position(x: frame.midX, y: frame.midY)
        .allowsHitTesting(false)
    }
    if selected.count == 1, let o = selected.first {
      let frame = screenFrame(o)
      Circle()
        .fill(Color.white)
        .overlay(Circle().stroke(Color.accentColor, lineWidth: 2))
        .frame(width: 18, height: 18)
        .frame(width: 44, height: 44)
        .contentShape(Rectangle())
        .gesture(
          DragGesture(minimumDistance: 1, coordinateSpace: .local)
            .onChanged { resizeBy = $0.translation }
            .onEnded { value in
              resize(o, by: value.translation)
              resizeBy = .zero
            })
        .position(x: frame.maxX + 3, y: frame.maxY + 3)
        .accessibilityLabel("Resize")
    }
  }

  private func resize(_ o: EaselObject, by translation: CGSize) {
    var after = o
    after.width = max(o.width + translation.width / zoom, 8)
    after.height = max(o.height + translation.height / zoom, 8)
    if o.hasShape, !o.shape.points.isEmpty {
      // Points stretch with the box.
      let sx = o.width > 0 ? after.width / o.width : 1, sy = o.height > 0 ? after.height / o.height : 1
      after.shape.points = o.shape.points.enumerated().map { $0.offset % 2 == 0 ? $0.element * sx : $0.element * sy }
    }
    apply([(o, after)])
  }

  @ViewBuilder private func objectMenu(_ o: EaselObject) -> some View {
    if o.hasLink || o.hasCapture {
      let url = o.hasLink ? o.link.url : o.capture.url
      Button("Open", systemImage: "safari") { if let u = URL(string: url) { app.openFromEasel(u) } }
      Button("Select", systemImage: "checkmark.circle") { selection = [o.uuid] }
    }
    if o.hasCapture {
      Button("Refresh", systemImage: "arrow.clockwise") { refresh(o) }
    }
    if o.hasText {
      Button("Edit Text", systemImage: "character.cursor.ibeam") { beginEditing(o, isNew: false) }
    }
    Button("Bring to Front", systemImage: "square.3.layers.3d.top.filled") { arrange([o.uuid], front: true) }
    Button("Send to Back", systemImage: "square.3.layers.3d.bottom.filled") { arrange([o.uuid], front: false) }
    Button("Duplicate", systemImage: "plus.square.on.square") { duplicate([o.uuid]) }
    Button("Delete", systemImage: "trash", role: .destructive) { delete([o.uuid]) }
  }

  // MARK: Text

  private func beginEditing(_ o: EaselObject, isNew: Bool) {
    finishEditing()
    selection = []
    editing = o
    editingIsNew = isNew
    editText = o.text.text
    Task {
      try? await Task.sleep(for: .milliseconds(50))
      editFocused = true
    }
  }

  private func textEditor(_ o: EaselObject) -> some View {
    let frame = CGRect(
      x: o.x * zoom + offset.x, y: o.y * zoom + offset.y, width: o.width * zoom,
      height: max(o.height * zoom, EaselObjectView.fontSize(o.text.style) * zoom * 1.6))
    return TextEditor(text: $editText)
      .font(EaselObjectView.font(o.text, zoom: zoom))
      .foregroundStyle(EaselColor.color(o.text.color, on: palette))
      .scrollContentBackground(.hidden)
      .scrollDisabled(true)
      .focused($editFocused)
      .padding(-5)
      .frame(width: max(frame.width, 60), height: max(frame.height, 30) + 8)
      .background(RoundedRectangle(cornerRadius: 4).stroke(Color.accentColor.opacity(0.6), lineWidth: 1))
      .position(x: frame.minX + max(frame.width, 60) / 2, y: frame.minY + (max(frame.height, 30) + 8) / 2)
      .accessibilityLabel("Text")
  }

  /// Saves the text being edited: a box as tall as its text, or deleted
  /// when it's empty.
  private func finishEditing() {
    guard var o = editing else { return }
    editing = nil
    editFocused = false
    let text = editText.trimmingCharacters(in: .whitespacesAndNewlines)
    let before = editingIsNew ? nil : store.object(o.uuid)
    if text.isEmpty {
      if let before { apply([(before, nil)]) }
      return
    }
    o.text.text = editText.trimmingCharacters(in: .newlines)
    o.height = Self.textHeight(o.text, width: o.width)
    if editingIsNew { o.z = topZ + 1 }
    if before?.text.text == o.text.text && before?.height == o.height { return }
    apply([(before, o)])
  }

  /// The height of `text` wrapped at `width`, in board points.
  static func textHeight(_ text: SyncPb_CompassEaselText, width: Double) -> Double {
    let size = EaselObjectView.fontSize(text.style)
    let font: UIFont =
      switch text.font {
      case "mono": .monospacedSystemFont(ofSize: size, weight: .regular)
      case "hand": UIFont(name: "Noteworthy-Light", size: size) ?? .systemFont(ofSize: size)
      default: .systemFont(ofSize: size, weight: text.style == "title" || text.style == "heading" ? .semibold : .regular)
      }
    let rect = (text.text as NSString).boundingRect(
      with: CGSize(width: max(width, 20), height: .greatestFiniteMagnitude), options: [.usesLineFragmentOrigin],
      attributes: [.font: font], context: nil)
    return max(ceil(rect.height) + 6, size * 1.4)
  }

  // MARK: Editing

  /// Applies changes (an object before and after; nil is none) and keeps
  /// them for Undo. Returns what was saved.
  @discardableResult
  private func apply(_ changes: [(EaselObject?, EaselObject?)]) -> [EaselObject?] {
    var done: [(EaselObject?, EaselObject?)] = []
    var result: [EaselObject?] = []
    let saves = changes.compactMap(\.1)
    var saved: [EaselObject] = []
    if !saves.isEmpty {
      switch store.save(saves) {
      case .success(let list): saved = list
      case .failure(let error):
        app.showMessage(error.message)
        return []
      }
    }
    var next = saved.makeIterator()
    for (before, after) in changes {
      if after != nil {
        let s = next.next()
        done.append((before, s))
        result.append(s)
      } else if let before {
        store.deleteObject(before.uuid)
        selection.remove(before.uuid)
        done.append((before, nil))
        result.append(nil)
      }
    }
    history.record(done)
    return result
  }

  private func arrange(_ ids: Set<String>, front: Bool) {
    let objects = store.objects(of: board).filter { ids.contains($0.uuid) }
    var z = front ? topZ : bottomZ
    apply(objects.map { before in
      var after = before
      z += front ? 1 : -1
      after.z = z
      return (before, after)
    })
  }

  private func duplicate(_ ids: Set<String>) {
    let objects = store.objects(of: board).filter { ids.contains($0.uuid) }
    var z = topZ
    let saved = apply(objects.map { o in
      var copy = o
      copy.uuid = ""
      copy.x += 20
      copy.y += 20
      z += 1
      copy.z = z
      return (nil, copy)
    })
    selection = Set(saved.compactMap { $0?.uuid })
  }

  private func delete(_ ids: Set<String>) {
    apply(store.objects(of: board).filter { ids.contains($0.uuid) }.map { ($0, nil) })
    selection = []
  }

  private func recolor(_ ids: Set<String>, _ color: EaselColor) {
    let objects = store.objects(of: board).filter { ids.contains($0.uuid) && ($0.hasText || $0.hasShape) }
    apply(objects.map { before in
      var after = before
      if after.hasText {
        if let css = color.css { after.text.color = css } else { after.text.clearColor() }
      } else {
        if let css = color.css { after.shape.color = css } else { after.shape.clearColor() }
      }
      return (before, after)
    })
  }

  private func setText(_ ids: Set<String>, style: String? = nil, font: String? = nil) {
    let objects = store.objects(of: board).filter { ids.contains($0.uuid) && $0.hasText }
    apply(objects.map { before in
      var after = before
      if let style { after.text.style = style }
      if let font { after.text.font = font }
      after.height = Self.textHeight(after.text, width: after.width)
      return (before, after)
    })
  }

  private func undo() {
    finishEditing()
    if let error = history.undo(store) { app.showMessage(error) }
    selection = selection.filter { store.object($0) != nil }
  }

  private func redo() {
    finishEditing()
    if let error = history.redo(store) { app.showMessage(error) }
    selection = selection.filter { store.object($0) != nil }
  }

  private func refresh(_ o: EaselObject) {
    refreshing.insert(o.uuid)
    Task {
      let result = await EaselCapture.refresh(o, tabs: app.tabs)
      refreshing.remove(o.uuid)
      if case .failure(let error) = result { app.showMessage(error.message) }
    }
  }

  /// Fits everything on screen (or the board's top left when it's empty).
  private func fit(_ objects: [EaselObject], animated: Bool = true) {
    let top = topInset + 60, bottom = bottomInset + 130
    let available = CGSize(width: max(viewSize.width - 40, 100), height: max(viewSize.height - top - bottom, 100))
    guard let minX = objects.map(\.x).min(), let minY = objects.map(\.y).min(),
      let maxX = objects.map({ $0.x + $0.width }).max(), let maxY = objects.map({ $0.y + $0.height }).max()
    else {
      zoom = 1
      offset = CGPoint(x: 24, y: top)
      rememberView()
      return
    }
    let bounds = CGRect(x: minX, y: minY, width: max(maxX - minX, 1), height: max(maxY - minY, 1))
    let newZoom = min(max(min(available.width / bounds.width, available.height / bounds.height), 0.1), 2)
    let change = {
      zoom = newZoom
      offset = CGPoint(
        x: (viewSize.width - bounds.width * newZoom) / 2 - bounds.minX * newZoom,
        y: top + (available.height - bounds.height * newZoom) / 2 - bounds.minY * newZoom)
    }
    if animated { withAnimation(.snappy) { change() } } else { change() }
    rememberView()
  }

  // MARK: Adding

  /// An image from Photos or the clipboard, fitted, in the middle of the
  /// view.
  private func addImage(_ data: Data) async {
    guard let fitted = await Task.detached(operation: { EaselImages.fit(data) }).value.success else {
      app.showMessage("The image couldn't be read.")
      return
    }
    switch store.addAsset(fitted.data, mimeType: fitted.mimeType) {
    case .failure(let error): app.showMessage(error.message)
    case .success(let sha):
      // At half its pixels (a 2× screen's points), at most 360 a side.
      var size = CGSize(width: Double(fitted.width) / 2, height: Double(fitted.height) / 2)
      let scale = min(1, 360 / max(size.width, size.height))
      size = CGSize(width: size.width * scale, height: size.height * scale)
      var o = EaselObject()
      o.easelUuid = board
      o.image.asset = sha
      o.image.pixelWidth = Int32(fitted.width)
      o.image.pixelHeight = Int32(fitted.height)
      place(&o, size: size)
      if let saved = apply([(nil, o)]).first ?? nil { selection = [saved.uuid] }
    }
  }

  private func place(_ o: inout EaselObject, size: CGSize) {
    let center = visibleCenter
    o.x = center.x - size.width / 2
    o.y = center.y - size.height / 2
    o.width = size.width
    o.height = size.height
    o.z = topZ + 1
  }

  /// Paste: an image, a URL as a link card, other text as text.
  private func paste() {
    let board = UIPasteboard.general
    if board.hasImages, let image = board.image, let data = image.pngData() {
      Task { await addImage(data) }
    } else if let url = board.url ?? board.string.flatMap(Self.webURL), ["http", "https"].contains(url.scheme ?? "") {
      addLink(url)
    } else if let text = board.string, !text.isEmpty {
      var o = EaselObject()
      o.easelUuid = self.board
      o.text.text = text
      if let css = color.css { o.text.color = css }
      place(&o, size: CGSize(width: 280, height: 30))
      o.height = Self.textHeight(o.text, width: 280)
      if let saved = apply([(nil, o)]).first ?? nil { selection = [saved.uuid] }
    } else {
      app.showMessage("There's nothing to paste.")
    }
  }

  static func webURL(_ text: String) -> URL? {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !trimmed.contains(" "), let url = URL(string: trimmed), url.host() != nil else { return nil }
    return url
  }

  /// A link card now (its host), then its page's title, description and
  /// image once they come.
  private func addLink(_ url: URL) {
    var o = EaselObject()
    o.easelUuid = board
    o.link.url = url.absoluteString
    o.link.title = url.host() ?? url.absoluteString
    place(&o, size: CGSize(width: 260, height: 90))
    guard let saved = apply([(nil, o)]).first ?? nil else { return }
    selection = [saved.uuid]
    Task {
      let (preview, image) = await EaselLinks.fetch(url, userAgent: Favicons.userAgent)
      guard var current = store.object(saved.uuid), current.hasLink else { return }
      if !preview.title.isEmpty { current.link.title = preview.title }
      if !preview.description.isEmpty { current.link.description_p = preview.description }
      if let image, let sha = try? store.addAsset(image.data, mimeType: image.mimeType).get() {
        current.link.imageAsset = sha
        // Room for the image, unless the card was resized meanwhile.
        if current.height == 90 { current.height = 230 }
      }
      _ = store.save(current)
    }
  }

  // MARK: Bars

  private var topBar: some View {
    HStack(spacing: 8) {
      TextField("Title", text: $title)
        .font(.headline)
        .disabled(!RoomsModel.shared.canEdit(easel: board))
        .focused($editTitleFocused)
        .submitLabel(.done)
        .onSubmit(rename)
        .onChange(of: editTitleFocused) { _, focused in if !focused { rename() } }
        .padding(.horizontal, 14)
        .frame(height: 40)
        .floatingGlass(in: Capsule(), frosted: true)
      if editing != nil {
        Button("Done") { finishEditing() }
          .font(.headline)
          .padding(.horizontal, 14)
          .frame(height: 40)
          .floatingGlass(in: Capsule(), frosted: true)
      }
    }
    .padding(.horizontal, 12)
  }

  private func rename() {
    let name = title.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let easel, !name.isEmpty, name != easel.title else {
      title = easel?.title ?? title
      return
    }
    store.renameEasel(board, to: name)
    app.easelRenamed(board, title: store.easel(board)?.title ?? name)
  }

  @ViewBuilder private func bottomBars(_ objects: [EaselObject]) -> some View {
    VStack(spacing: 8) {
      if !RoomsModel.shared.canEdit(easel: board) {
        // Shared with you to view (App/Rooms): no tools.
        Button { RoomsModel.shared.share(easel: board) } label: {
          Label("View Only", systemImage: "eye")
            .font(.subheadline.weight(.medium))
            .padding(.horizontal, 16)
            .frame(height: 40)
            .floatingGlass(in: Capsule(), frosted: true)
        }
        .buttonStyle(.plain)
      } else {
        if !selection.isEmpty { selectionBar(objects) }
        toolBar(objects)
      }
    }
    .padding(.horizontal, 12)
  }

  private func selectionBar(_ objects: [EaselObject]) -> some View {
    let selected = objects.filter { selection.contains($0.uuid) }
    let texts = selected.filter(\.hasText)
    return HStack(spacing: 2) {
      if selected.contains(where: { $0.hasText || $0.hasShape }) {
        Menu {
          ForEach(EaselColor.allCases) { c in
            Button(c.rawValue.capitalized) { recolor(selection, c) }
          }
        } label: {
          BarGlyph(symbol: "paintpalette")
        }
        .accessibilityLabel("Color")
      }
      if !texts.isEmpty {
        Menu {
          Section("Style") {
            ForEach(["title", "heading", "body", "caption"], id: \.self) { s in
              Button(s.capitalized) { setText(selection, style: s) }
            }
          }
          Section("Font") {
            ForEach(["sans", "serif", "mono", "hand"], id: \.self) { f in
              Button(f == "hand" ? "Handwriting" : f.capitalized) { setText(selection, font: f) }
            }
          }
        } label: {
          BarGlyph(symbol: "textformat.size")
        }
        .accessibilityLabel("Text style")
      }
      if selected.count == 1, let o = selected.first, o.hasText {
        Button { beginEditing(o, isNew: false) } label: { BarGlyph(symbol: "character.cursor.ibeam") }
          .accessibilityLabel("Edit text")
      }
      if selected.count == 1, let o = selected.first, o.hasCapture {
        Button { refresh(o) } label: { BarGlyph(symbol: "arrow.clockwise") }
          .accessibilityLabel("Refresh")
      }
      Button { arrange(selection, front: true) } label: { BarGlyph(symbol: "square.3.layers.3d.top.filled") }
        .accessibilityLabel("Bring to front")
      Button { arrange(selection, front: false) } label: { BarGlyph(symbol: "square.3.layers.3d.bottom.filled") }
        .accessibilityLabel("Send to back")
      Button { duplicate(selection) } label: { BarGlyph(symbol: "plus.square.on.square") }
        .accessibilityLabel("Duplicate")
      Button { delete(selection) } label: { BarGlyph(symbol: "trash") }
        .accessibilityLabel("Delete")
    }
    .buttonStyle(.plain)
    .padding(4)
    .floatingGlass(in: Capsule(), frosted: true)
  }

  private func toolBar(_ objects: [EaselObject]) -> some View {
    HStack(spacing: 0) {
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 0) {
          ForEach([Tool.select, .text, .rectangle, .circle, .arrow, .pen]) { t in
            Button {
              finishEditing()
              tool = t
            } label: {
              BarGlyph(symbol: t.symbol, on: tool == t)
            }
            .accessibilityLabel(t.title)
            .accessibilityAddTraits(tool == t ? .isSelected : [])
          }
          PhotosPicker(selection: $photo, matching: .images) { BarGlyph(symbol: "photo") }
            .accessibilityLabel("Add image")
        }
      }
      .scrollBounceBehavior(.basedOnSize)
      Menu {
        ForEach(EaselColor.allCases) { c in
          Button {
            color = c
          } label: {
            Label(c.rawValue.capitalized, systemImage: color == c ? "checkmark.circle.fill" : "circle.fill")
          }
        }
      } label: {
        Circle().fill(color.color(on: palette))
          .overlay(Circle().stroke(Color.primary.opacity(0.2), lineWidth: 1))
          .frame(width: 22, height: 22)
          .frame(width: 40, height: 40)
      }
      .accessibilityLabel("Color")
      Menu {
        Button("Paste", systemImage: "doc.on.clipboard") { paste() }
        Button("Select Area", systemImage: "rectangle.dashed") { tool = .area }
        Button("Fit", systemImage: "arrow.up.left.and.arrow.down.right") { fit(objects) }
        Section {
          Button("Undo", systemImage: "arrow.uturn.backward") { undo() }.disabled(!history.canUndo)
          Button("Redo", systemImage: "arrow.uturn.forward") { redo() }.disabled(!history.canRedo)
        }
        Picker("Background", selection: Binding(get: { palette }, set: { store.setPalette(board, $0.rawValue) })) {
          ForEach(EaselPalette.allCases) { Text($0.title).tag($0) }
        }
        .pickerStyle(.menu)
        Button("All Easels", systemImage: "rectangle.stack") { app.store.update(tab, url: EaselURL.list, title: "Easels") }
        if RoomsModel.shared.isAvailable {
          Button(RoomsModel.shared.room(ofEasel: board) == nil ? "Share…" : "Sharing…", systemImage: "person.crop.circle.badge.plus") {
            RoomsModel.shared.share(easel: board)
          }
        }
      } label: {
        BarGlyph(symbol: "ellipsis.circle")
      }
      .accessibilityLabel("More")
      Button { undo() } label: { BarGlyph(symbol: "arrow.uturn.backward") }
        .disabled(!history.canUndo)
        .opacity(history.canUndo ? 1 : 0.35)
        .accessibilityLabel("Undo")
    }
    .buttonStyle(.plain)
    .padding(4)
    .floatingGlass(in: Capsule(), frosted: true)
  }
}

extension EaselEditor.Tool: Identifiable {
  var id: String { rawValue }
}

/// A glyph on the editor's bars.
private struct BarGlyph: View {
  let symbol: String
  var on = false

  var body: some View {
    Image(systemName: symbol)
      .font(.system(size: 17, weight: .semibold))
      .foregroundStyle(on ? Color.accentColor : .primary)
      .frame(width: 40, height: 40)
      .background(on ? Color.accentColor.opacity(0.15) : .clear, in: Circle())
      .contentShape(Circle())
  }
}

/// The editor's Undo and Redo: lists of changes, each an object before and
/// after (nil: not there).
@MainActor
@Observable
final class EaselHistory {
  typealias Change = (before: EaselObject?, after: EaselObject?)
  private var undoStack: [[Change]] = []
  private var redoStack: [[Change]] = []
  var canUndo: Bool { !undoStack.isEmpty }
  var canRedo: Bool { !redoStack.isEmpty }

  func record(_ changes: [Change]) {
    guard !changes.isEmpty else { return }
    undoStack.append(changes)
    if undoStack.count > 100 { undoStack.removeFirst() }
    redoStack.removeAll()
  }

  /// Puts back what the last change replaced; an error in words, if any.
  func undo(_ store: EaselStore) -> String? {
    guard let changes = undoStack.popLast() else { return nil }
    redoStack.append(changes)
    return Self.put(changes.map { ($0.after, $0.before) }, in: store)
  }

  func redo(_ store: EaselStore) -> String? {
    guard let changes = redoStack.popLast() else { return nil }
    undoStack.append(changes)
    return Self.put(changes, in: store)
  }

  private static func put(_ changes: [Change], in store: EaselStore) -> String? {
    var error: String?
    for (before, after) in changes {
      if let after {
        if case .failure(let e) = store.save(after) { error = e.message }
      } else if let before {
        store.deleteObject(before.uuid)
      }
    }
    return error
  }
}

extension CGRect {
  init(corners a: CGPoint, _ b: CGPoint) {
    self.init(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
  }
}
