// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/easel/link_metadata.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::easel {
namespace {

constexpr char kPageUrl[] = "https://example.com/blog/post?id=1";

TEST(LinkMetadataTest, OpenGraphFirst) {
  const LinkMetadata metadata = ParseLinkMetadata(R"(<!doctype html>
<html><head>
<title>Page title</title>
<meta name="description" content="Plain description">
<meta property="og:title" content="Tom &amp; Jerry&#39;s &quot;Show&quot;">
<meta property="og:description" content="The &lt;best&gt; one">
<meta property="og:image" content="/images/card.png">
</head><body></body></html>)",
                                                  GURL(kPageUrl));
  EXPECT_EQ(metadata.title, "Tom & Jerry's \"Show\"");
  EXPECT_EQ(metadata.description, "The <best> one");
  EXPECT_EQ(metadata.image_url, GURL("https://example.com/images/card.png"));
}

TEST(LinkMetadataTest, FallsBackToTitleAndDescription) {
  const LinkMetadata metadata = ParseLinkMetadata(R"(<HTML><HEAD>
<TITLE>
  A   title
  over lines &mdash; ok
</TITLE>
<META NAME=description CONTENT='Single quoted'>
<meta name="twitter:image" content="img/t.jpg">
</HEAD>)",
                                                  GURL(kPageUrl));
  EXPECT_EQ(metadata.title, "A title over lines \xE2\x80\x94 ok");
  EXPECT_EQ(metadata.description, "Single quoted");
  EXPECT_EQ(metadata.image_url, GURL("https://example.com/blog/img/t.jpg"));
}

TEST(LinkMetadataTest, TwitterWhenNoOpenGraph) {
  const LinkMetadata metadata = ParseLinkMetadata(
      R"(<title>Fallback</title>
<meta name="twitter:title" content="Tweet title">
<meta name="twitter:description" content="Tweet description">)",
      GURL(kPageUrl));
  EXPECT_EQ(metadata.title, "Tweet title");
  EXPECT_EQ(metadata.description, "Tweet description");
}

TEST(LinkMetadataTest, IgnoresScriptsCommentsAndTheBody) {
  const LinkMetadata metadata = ParseLinkMetadata(R"(<head>
<!-- <meta property="og:title" content="In a comment"> -->
<script>var s = '<meta property="og:title" content="In a script">';</script>
<title>Real</title>
</head>
<body><meta property="og:description" content="In the body"></body>)",
                                                  GURL(kPageUrl));
  EXPECT_EQ(metadata.title, "Real");
  EXPECT_EQ(metadata.description, "");
}

TEST(LinkMetadataTest, OnlyWebImages) {
  EXPECT_FALSE(ParseLinkMetadata(
                   R"html(<meta property="og:image" content="javascript:alert(1)">)html",
                   GURL(kPageUrl))
                   .image_url.is_valid());
  EXPECT_FALSE(ParseLinkMetadata(
                   R"(<meta property="og:image" content="data:image/png;base64,AA">)",
                   GURL(kPageUrl))
                   .image_url.is_valid());
  EXPECT_EQ(ParseLinkMetadata(
                R"(<meta property="og:image" content=" //cdn.example.com/a.png ">)",
                GURL(kPageUrl))
                .image_url,
            GURL("https://cdn.example.com/a.png"));
}

TEST(LinkMetadataTest, FirstOfEachWins) {
  const LinkMetadata metadata = ParseLinkMetadata(
      R"(<meta property="og:title" content="First">
<meta property="og:title" content="Second">)",
      GURL(kPageUrl));
  EXPECT_EQ(metadata.title, "First");
}

TEST(LinkMetadataTest, BrokenHtmlDoesntHang) {
  EXPECT_EQ(ParseLinkMetadata("<title>Unclosed", GURL(kPageUrl)).title, "");
  EXPECT_EQ(ParseLinkMetadata("<meta content=\"no end", GURL(kPageUrl)).title, "");
  EXPECT_EQ(ParseLinkMetadata("<<<>>> < <!-- ", GURL(kPageUrl)).title, "");
  EXPECT_EQ(ParseLinkMetadata("", GURL(kPageUrl)).title, "");
}

TEST(LinkMetadataTest, MetaCharset) {
  EXPECT_EQ(FindMetaCharset(R"(<head><meta charset="ISO-8859-1">)"),
            "iso-8859-1");
  EXPECT_EQ(
      FindMetaCharset(R"(<meta http-equiv="Content-Type" )"
                      R"(content="text/html; charset=Shift_JIS">)"),
      "shift_jis");
  EXPECT_EQ(FindMetaCharset("<title>none</title>"), "");
}

TEST(LinkMetadataTest, Entities) {
  EXPECT_EQ(DecodeHtmlEntities("a &amp; b"), "a & b");
  EXPECT_EQ(DecodeHtmlEntities("&#x41;&#66;&#X43;"), "ABC");
  EXPECT_EQ(DecodeHtmlEntities("&#x1F600;"), "\xF0\x9F\x98\x80");
  // Unknown, unterminated or invalid ones stay.
  EXPECT_EQ(DecodeHtmlEntities("&bogus; & &#xD800; &#;"),
            "&bogus; & &#xD800; &#;");
}

}  // namespace
}  // namespace compass::easel
