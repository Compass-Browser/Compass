// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/easel/link_metadata.h"

#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "base/functional/function_ref.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"

namespace compass::easel {
namespace {

// What a card can show; longer text is cut.
constexpr size_t kMaxTitleBytes = 600;
constexpr size_t kMaxDescriptionBytes = 2000;

using Attributes = std::map<std::string, std::string>;

bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsTagNameChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '-' || c == ':';
}

size_t FindCaseInsensitive(std::string_view text,
                           std::string_view needle,
                           size_t from) {
  if (needle.empty() || text.size() < needle.size()) {
    return std::string_view::npos;
  }
  for (size_t i = from; i + needle.size() <= text.size(); ++i) {
    if (base::EqualsCaseInsensitiveASCII(text.substr(i, needle.size()),
                                         needle)) {
      return i;
    }
  }
  return std::string_view::npos;
}

// Calls |on_tag| for each start tag of the head with its name (lowercase),
// attributes (names lowercase, values decoded; the first of a name wins)
// and, for <title>, its text. Stops at <body> or </head>, or when |on_tag|
// returns false.
void ScanHead(
    std::string_view html,
    base::FunctionRef<bool(const std::string& name,
                           const Attributes& attributes,
                           std::string_view text)> on_tag) {
  size_t pos = 0;
  while ((pos = html.find('<', pos)) != std::string_view::npos) {
    if (html.substr(pos).starts_with("<!--")) {
      const size_t end = html.find("-->", pos + 4);
      if (end == std::string_view::npos) {
        return;
      }
      pos = end + 3;
      continue;
    }
    size_t i = pos + 1;
    bool closing = false;
    if (i < html.size() && html[i] == '/') {
      closing = true;
      ++i;
    }
    const size_t name_start = i;
    while (i < html.size() && IsTagNameChar(html[i])) {
      ++i;
    }
    if (i == name_start) {
      // "<" in text, "<!doctype", "<?xml".
      pos = i;
      continue;
    }
    const std::string name =
        base::ToLowerASCII(html.substr(name_start, i - name_start));

    Attributes attributes;
    while (i < html.size()) {
      while (i < html.size() && (IsHtmlSpace(html[i]) || html[i] == '/')) {
        ++i;
      }
      if (i >= html.size() || html[i] == '>') {
        break;
      }
      const size_t attr_start = i;
      while (i < html.size() && !IsHtmlSpace(html[i]) && html[i] != '=' &&
             html[i] != '>' && html[i] != '/') {
        ++i;
      }
      const std::string attr_name =
          base::ToLowerASCII(html.substr(attr_start, i - attr_start));
      while (i < html.size() && IsHtmlSpace(html[i])) {
        ++i;
      }
      std::string_view value;
      if (i < html.size() && html[i] == '=') {
        ++i;
        while (i < html.size() && IsHtmlSpace(html[i])) {
          ++i;
        }
        if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
          const char quote = html[i];
          const size_t end = html.find(quote, i + 1);
          if (end == std::string_view::npos) {
            return;
          }
          value = html.substr(i + 1, end - i - 1);
          i = end + 1;
        } else {
          const size_t value_start = i;
          while (i < html.size() && !IsHtmlSpace(html[i]) && html[i] != '>') {
            ++i;
          }
          value = html.substr(value_start, i - value_start);
        }
      }
      if (!attr_name.empty()) {
        attributes.emplace(attr_name, DecodeHtmlEntities(value));
      }
    }
    if (i >= html.size()) {
      return;
    }
    pos = i + 1;

    if (closing) {
      if (name == "head") {
        return;
      }
      continue;
    }
    if (name == "body") {
      return;
    }
    std::string_view text;
    if (name == "script" || name == "style" || name == "title" ||
        name == "noscript" || name == "template") {
      // Raw text up to the end tag: a "<meta" in a script isn't one.
      const size_t end = FindCaseInsensitive(html, "</" + name, pos);
      if (end == std::string_view::npos) {
        return;
      }
      text = html.substr(pos, end - pos);
      pos = end;
    }
    if (!on_tag(name, attributes, text)) {
      return;
    }
  }
}

std::string Attribute(const Attributes& attributes, const std::string& name) {
  auto it = attributes.find(name);
  return it == attributes.end() ? std::string() : it->second;
}

// |decode|: |text| is raw (a title's), not an attribute value, which the
// scanner decoded already.
std::string CleanText(std::string_view text, size_t max_bytes, bool decode) {
  std::string decoded =
      decode ? DecodeHtmlEntities(text) : std::string(text);
  // A no-break space is a space on a card.
  base::ReplaceSubstringsAfterOffset(&decoded, 0, "\xC2\xA0", " ");
  std::string collapsed = base::CollapseWhitespaceASCII(decoded, false);
  std::string truncated;
  base::TruncateUTF8ToByteSize(collapsed, max_bytes, &truncated);
  return truncated;
}

std::optional<std::string_view> NamedEntity(std::string_view name) {
  static constexpr std::pair<std::string_view, std::string_view> kEntities[] =
      {
          {"amp", "&"},          {"lt", "<"},
          {"gt", ">"},           {"quot", "\""},
          {"apos", "'"},         {"nbsp", "\xC2\xA0"},
          {"hellip", "\xE2\x80\xA6"}, {"mdash", "\xE2\x80\x94"},
          {"ndash", "\xE2\x80\x93"},  {"lsquo", "\xE2\x80\x98"},
          {"rsquo", "\xE2\x80\x99"},  {"ldquo", "\xE2\x80\x9C"},
          {"rdquo", "\xE2\x80\x9D"},  {"middot", "\xC2\xB7"},
          {"bull", "\xE2\x80\xA2"},   {"copy", "\xC2\xA9"},
          {"reg", "\xC2\xAE"},        {"trade", "\xE2\x84\xA2"},
      };
  for (const auto& [entity, text] : kEntities) {
    if (entity == name) {
      return text;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string DecodeHtmlEntities(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '&') {
      out.push_back(text[i++]);
      continue;
    }
    // At most "&#x10ffff;" or a short name.
    const size_t end = text.find(';', i + 1);
    if (end == std::string_view::npos || end - i > 10) {
      out.push_back(text[i++]);
      continue;
    }
    const std::string_view name = text.substr(i + 1, end - i - 1);
    if (name.size() > 1 && name[0] == '#') {
      uint32_t code = 0;
      const bool hex = name[1] == 'x' || name[1] == 'X';
      const std::string_view digits = name.substr(hex ? 2 : 1);
      bool ok = !digits.empty();
      for (char c : digits) {
        const bool digit =
            hex ? base::IsHexDigit(c) : base::IsAsciiDigit(c);
        if (!digit || code > 0x10FFFF) {
          ok = false;
          break;
        }
        code = code * (hex ? 16 : 10) + base::HexDigitToInt(c);
      }
      if (ok && code > 0 && code <= 0x10FFFF &&
          !(code >= 0xD800 && code <= 0xDFFF)) {
        base::WriteUnicodeCharacter(static_cast<base_icu::UChar32>(code), &out);
        i = end + 1;
        continue;
      }
    } else if (std::optional<std::string_view> named =
                   NamedEntity(base::ToLowerASCII(name))) {
      out.append(*named);
      i = end + 1;
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

std::string FindMetaCharset(std::string_view html) {
  std::string charset;
  ScanHead(html, [&charset](const std::string& name,
                            const Attributes& attributes,
                            std::string_view text) {
    if (name != "meta") {
      return true;
    }
    charset = Attribute(attributes, "charset");
    if (charset.empty() &&
        base::EqualsCaseInsensitiveASCII(
            Attribute(attributes, "http-equiv"), "content-type")) {
      const std::string content = Attribute(attributes, "content");
      const size_t at = FindCaseInsensitive(content, "charset=", 0);
      if (at != std::string::npos) {
        charset = content.substr(at + 8);
      }
    }
    base::TrimString(charset, " \t\"';", &charset);
    return charset.empty();
  });
  return base::ToLowerASCII(charset);
}

LinkMetadata ParseLinkMetadata(std::string_view html, const GURL& page_url) {
  // By preference; the first of each found wins.
  std::string og_title, twitter_title, title_tag;
  std::string og_description, twitter_description, meta_description;
  std::string og_image, twitter_image;
  ScanHead(html, [&](const std::string& name, const Attributes& attributes,
                     std::string_view text) {
    if (name == "title") {
      if (title_tag.empty()) {
        title_tag = CleanText(text, kMaxTitleBytes, /*decode=*/true);
      }
      return true;
    }
    if (name != "meta") {
      return true;
    }
    std::string key = Attribute(attributes, "property");
    if (key.empty()) {
      key = Attribute(attributes, "name");
    }
    key = base::ToLowerASCII(key);
    const std::string content = Attribute(attributes, "content");
    if (content.empty()) {
      return true;
    }
    auto keep = [&content](std::string& field) {
      if (field.empty()) {
        field = content;
      }
    };
    if (key == "og:title") {
      keep(og_title);
    } else if (key == "twitter:title") {
      keep(twitter_title);
    } else if (key == "og:description") {
      keep(og_description);
    } else if (key == "twitter:description") {
      keep(twitter_description);
    } else if (key == "description") {
      keep(meta_description);
    } else if (key == "og:image" || key == "og:image:url" ||
               key == "og:image:secure_url") {
      keep(og_image);
    } else if (key == "twitter:image" || key == "twitter:image:src") {
      keep(twitter_image);
    }
    return true;
  });

  auto first = [](std::initializer_list<const std::string*> candidates) {
    for (const std::string* candidate : candidates) {
      if (!candidate->empty()) {
        return *candidate;
      }
    }
    return std::string();
  };
  LinkMetadata result;
  result.title = CleanText(first({&og_title, &twitter_title}),
                           kMaxTitleBytes, /*decode=*/false);
  if (result.title.empty()) {
    result.title = title_tag;
  }
  result.description =
      CleanText(first({&og_description, &twitter_description,
                       &meta_description}),
                kMaxDescriptionBytes, /*decode=*/false);
  const std::string image = first({&og_image, &twitter_image});
  if (!image.empty()) {
    std::string trimmed;
    base::TrimWhitespaceASCII(image, base::TRIM_ALL, &trimmed);
    const GURL image_url = page_url.Resolve(trimmed);
    if (image_url.is_valid() && image_url.SchemeIsHTTPOrHTTPS()) {
      result.image_url = image_url;
    }
  }
  return result;
}

}  // namespace compass::easel
