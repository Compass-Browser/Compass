// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import QuickLook
import SwiftUI

/// Settings → Downloads: running and finished downloads. Tap to preview,
/// swipe to delete, long press to share. The same files are in the Files
/// app, under Compass Browser → Downloads.
struct DownloadsView: View {
  @Environment(AppModel.self) private var app
  @State private var preview: URL?

  var body: some View {
    let downloads = app.downloads
    List {
      if downloads.items.isEmpty {
        Text("Nothing downloaded yet").foregroundStyle(.secondary)
      }
      ForEach(downloads.items) { item in
        Button {
          preview = item.file
        } label: {
          HStack(spacing: 12) {
            Image(systemName: icon(item)).font(.title3).frame(width: 30)
            VStack(alignment: .leading, spacing: 4) {
              Text(item.name).lineLimit(1).foregroundStyle(.primary)
              if item.running {
                if let progress = item.progress { ProgressView(value: progress) } else { ProgressView() }
              } else if let failed = item.failed {
                Text(failed).font(.caption).foregroundStyle(.red).lineLimit(2)
              } else {
                Text("\(ByteCountFormatter.string(fromByteCount: item.bytes, countStyle: .file)) · \(item.date.formatted(.relative(presentation: .named)))")
                  .font(.caption).foregroundStyle(.secondary)
              }
            }
          }
        }
        .disabled(item.file == nil)
        .contextMenu {
          if let file = item.file {
            ShareLink(item: file)
          }
          Button("Delete", systemImage: "trash", role: .destructive) { downloads.delete(item) }
        }
        .swipeActions {
          Button("Delete", role: .destructive) { downloads.delete(item) }
        }
      }
    }
    .navigationTitle("Downloads")
    .quickLookPreview($preview)
    .onAppear { downloads.reload() }
  }

  private func icon(_ item: Downloads.Item) -> String {
    if item.failed != nil { return "exclamationmark.triangle" }
    if item.running { return "arrow.down.circle" }
    switch (item.name as NSString).pathExtension.lowercased() {
    case "pdf": return "doc.richtext"
    case "zip", "gz", "tar", "7z", "rar": return "doc.zipper"
    case "jpg", "jpeg", "png", "gif", "heic", "webp": return "photo"
    case "mp4", "mov", "m4v", "webm": return "film"
    case "mp3", "m4a", "wav", "aac": return "music.note"
    default: return "doc"
    }
  }
}
