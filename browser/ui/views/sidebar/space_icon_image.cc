// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/space_icon_image.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/space_icons_ts.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPoint.h"
#include "third_party/skia/include/utils/SkParsePath.h"
#include "ui/color/color_id.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/canvas_image_source.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/vector_icon_types.h"

namespace compass {

namespace {

// The symbols' view box, "0 -960 960 960" (SPACE_SYMBOL_VIEW_BOX).
constexpr SkScalar kSymbolCanvas = 960;

// The SVG path of symbol |name| in space_icons.ts, which holds the symbols
// for the page: `name: '<name>'`, then `path: '<path>'`. Empty if it isn't
// there.
std::string FindSymbolPath(std::string_view name) {
  const std::string_view source = space_icons_ts::kSpaceIcons;
  const std::string name_key = "name: '" + std::string(name) + "'";
  const size_t entry = source.find(name_key);
  if (entry == std::string_view::npos) {
    return std::string();
  }
  constexpr std::string_view kPathKey = "path: '";
  const size_t path = source.find(kPathKey, entry + name_key.size());
  const size_t next_entry = source.find("name: '", entry + name_key.size());
  if (path == std::string_view::npos ||
      (next_entry != std::string_view::npos && path > next_entry)) {
    return std::string();
  }
  const size_t start = path + kPathKey.size();
  const size_t end = source.find('\'', start);
  if (end == std::string_view::npos) {
    return std::string();
  }
  return std::string(source.substr(start, end - start));
}

// A symbol as a gfx::VectorIcon, made once and kept for good: icons are
// cached by address, so they must not go away.
struct SymbolIcon {
  std::vector<gfx::PathElement> path;
  gfx::VectorIconRep rep;
  std::unique_ptr<gfx::VectorIcon> icon;
  std::string name;
};

void AddPoint(std::vector<gfx::PathElement>& path, const SkPoint& point) {
  path.emplace_back(point.x());
  // The view box starts at y = -960.
  path.emplace_back(point.y() + kSymbolCanvas);
}

// |svg| as vector icon commands, in absolute coordinates: Skia parses every
// SVG command (arcs become conics, which become quadratics here).
std::optional<std::vector<gfx::PathElement>> ToVectorCommands(
    const std::string& svg) {
  std::optional<SkPath> parsed = SkParsePath::FromSVGString(svg.c_str());
  if (!parsed) {
    return std::nullopt;
  }
  std::vector<gfx::PathElement> path;
  path.emplace_back(gfx::CANVAS_DIMENSIONS);
  path.emplace_back(kSymbolCanvas);
  SkPath::Iter iter(*parsed, /*forceClose=*/false);
  SkPoint points[4];
  for (SkPath::Verb verb = iter.next(points); verb != SkPath::kDone_Verb;
       verb = iter.next(points)) {
    switch (verb) {
      case SkPath::kMove_Verb:
        path.emplace_back(gfx::MOVE_TO);
        AddPoint(path, points[0]);
        break;
      case SkPath::kLine_Verb:
        path.emplace_back(gfx::LINE_TO);
        AddPoint(path, points[1]);
        break;
      case SkPath::kQuad_Verb:
        path.emplace_back(gfx::QUADRATIC_TO);
        AddPoint(path, points[1]);
        AddPoint(path, points[2]);
        break;
      case SkPath::kConic_Verb: {
        // 2^2 quadratics are plenty at menu sizes.
        constexpr int kPow2 = 2;
        std::array<SkPoint, 1 + 2 * (1 << kPow2)> quads;
        const int count = SkPath::ConvertConicToQuads(
            points[0], points[1], points[2], iter.conicWeight(), quads.data(),
            kPow2);
        for (int i = 0; i < count; ++i) {
          path.emplace_back(gfx::QUADRATIC_TO);
          AddPoint(path, quads[1 + 2 * i]);
          AddPoint(path, quads[2 + 2 * i]);
        }
        break;
      }
      case SkPath::kCubic_Verb:
        path.emplace_back(gfx::CUBIC_TO);
        AddPoint(path, points[1]);
        AddPoint(path, points[2]);
        AddPoint(path, points[3]);
        break;
      case SkPath::kClose_Verb:
        path.emplace_back(gfx::CLOSE);
        break;
      case SkPath::kDone_Verb:
        break;
    }
  }
  return path;
}

const gfx::VectorIcon* GetSymbolIcon(const std::string& name) {
  static base::NoDestructor<std::map<std::string, std::unique_ptr<SymbolIcon>>>
      icons;
  auto it = icons->find(name);
  if (it != icons->end()) {
    return it->second ? it->second->icon.get() : nullptr;
  }
  std::unique_ptr<SymbolIcon> symbol;
  const std::string svg = FindSymbolPath(name);
  std::optional<std::vector<gfx::PathElement>> commands =
      svg.empty() ? std::nullopt : ToVectorCommands(svg);
  if (commands) {
    symbol = std::make_unique<SymbolIcon>();
    symbol->name = "compass_space_" + name;
    symbol->path = std::move(*commands);
    symbol->rep.path = base::span<const gfx::PathElement>(symbol->path);
    symbol->icon = std::make_unique<gfx::VectorIcon>(&symbol->rep, 1u,
                                                     symbol->name.c_str());
  }
  const gfx::VectorIcon* icon = symbol ? symbol->icon.get() : nullptr;
  icons->emplace(name, std::move(symbol));
  return icon;
}

// An emoji, centered in a square.
class EmojiSource : public gfx::CanvasImageSource {
 public:
  EmojiSource(std::u16string emoji, int size)
      : gfx::CanvasImageSource(gfx::Size(size, size)),
        emoji_(std::move(emoji)) {}
  EmojiSource(const EmojiSource&) = delete;
  EmojiSource& operator=(const EmojiSource&) = delete;
  ~EmojiSource() override = default;

  // gfx::CanvasImageSource:
  void Draw(gfx::Canvas* canvas) override {
    const gfx::FontList base;
    // The glyph a little smaller than the square, as the switcher shows it.
    const gfx::FontList font =
        base.DeriveWithSizeDelta(size().height() - 3 - base.GetFontSize());
    canvas->DrawStringRectWithFlags(emoji_, font, SK_ColorBLACK,
                                    gfx::Rect(size()),
                                    gfx::Canvas::TEXT_ALIGN_CENTER);
  }

 private:
  const std::u16string emoji_;
};

}  // namespace

ui::ImageModel GetSpaceIconImage(const SpaceIcon& icon, int size) {
  switch (icon.kind()) {
    case SpaceIcon::Kind::kNone:
      return ui::ImageModel();
    case SpaceIcon::Kind::kSymbol: {
      const gfx::VectorIcon* vector_icon = GetSymbolIcon(icon.value());
      return vector_icon ? ui::ImageModel::FromVectorIcon(
                               *vector_icon, ui::kColorMenuIcon, size)
                         : ui::ImageModel();
    }
    case SpaceIcon::Kind::kEmoji:
      return ui::ImageModel::FromImageSkia(
          gfx::CanvasImageSource::MakeImageSkia<EmojiSource>(
              base::UTF8ToUTF16(icon.value()), size));
  }
  NOTREACHED();
}

}  // namespace compass
