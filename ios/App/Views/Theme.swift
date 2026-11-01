// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

extension SpaceColor {
  var color: Color {
    switch self {
    case .grey: Color(red: 0.55, green: 0.57, blue: 0.62)
    case .blue: Color(red: 0.33, green: 0.47, blue: 0.95)
    case .purple: Color(red: 0.55, green: 0.38, blue: 0.93)
    case .pink: Color(red: 0.93, green: 0.36, blue: 0.62)
    case .red: Color(red: 0.92, green: 0.30, blue: 0.30)
    case .orange: Color(red: 0.96, green: 0.55, blue: 0.22)
    case .yellow: Color(red: 0.93, green: 0.75, blue: 0.18)
    case .green: Color(red: 0.25, green: 0.70, blue: 0.45)
    }
  }

  var name: String {
    switch self {
    case .grey: "Grey"
    case .blue: "Blue"
    case .purple: "Purple"
    case .pink: "Pink"
    case .red: "Red"
    case .orange: "Orange"
    case .yellow: "Yellow"
    case .green: "Green"
    }
  }
}

/// A site's icon, from its favicon service-free origin (the first letter
/// while it loads or when there's none).
struct SiteIcon: View {
  let url: String
  var size: CGFloat = 18

  var body: some View {
    let host = URL(string: url)?.host() ?? ""
    AsyncImage(url: URL(string: "https://\(host)/favicon.ico")) { phase in
      if let image = phase.image {
        image.resizable().interpolation(.high)
      } else {
        Text(host.replacingOccurrences(of: "www.", with: "").prefix(1).uppercased())
          .font(.system(size: size * 0.6, weight: .semibold))
          .frame(maxWidth: .infinity, maxHeight: .infinity)
          .background(.quaternary, in: RoundedRectangle(cornerRadius: size * 0.25))
      }
    }
    .frame(width: size, height: size)
    .clipShape(RoundedRectangle(cornerRadius: size * 0.25))
  }
}
