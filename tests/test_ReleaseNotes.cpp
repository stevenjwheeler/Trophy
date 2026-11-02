/*
 * Trophy - Unit Tests
 * ReleaseNotes: GitHub release Markdown -> themed Qt rich text.
 */

#include <QColor>
#include <QString>
#include <gtest/gtest.h>

#include "core/ReleaseNotes.h"

using namespace TrophyBackend;

namespace {

const QColor kAccent(QStringLiteral("#e94560"));
const QColor kCard(QStringLiteral("#090909"));

QString render(const QString &markdown) {
  return releaseNotesHtml(markdown, kAccent, kCard);
}

} // namespace

TEST(ReleaseNotesTest, EmptyInputIsEmpty) {
  EXPECT_TRUE(render(QString()).isEmpty());
  EXPECT_TRUE(render(QStringLiteral("\n\n  \n")).isEmpty());
}

TEST(ReleaseNotesTest, HeadingsUseThemeSizesAndAccent) {
  const QString html =
      render(QStringLiteral("# Title\n\n## What's Changed\n\n### Fixes"));

  EXPECT_TRUE(html.contains(QStringLiteral("<h1")));
  EXPECT_TRUE(html.contains(QStringLiteral("font-size:24px")));
  EXPECT_TRUE(html.contains(QStringLiteral("<h2")));
  EXPECT_TRUE(html.contains(QStringLiteral("font-size:20px")));
  EXPECT_TRUE(html.contains(QStringLiteral("<h3")));
  EXPECT_TRUE(html.contains(QStringLiteral("font-size:16px")));
  EXPECT_TRUE(html.contains(QStringLiteral(">Title</h1>")));
  EXPECT_TRUE(html.contains(QStringLiteral(">What's Changed</h2>")));
  EXPECT_TRUE(html.contains(QStringLiteral(">Fixes</h3>")));
  // Top-level headings are tinted; lower headings inherit the body color.
  EXPECT_EQ(html.count(QStringLiteral("#e94560")), 2);
}

TEST(ReleaseNotesTest, ParagraphsJoinSoftLinesAndKeepHardBreaks) {
  const QString soft = render(QStringLiteral("one\ntwo\n\nthree"));
  EXPECT_TRUE(soft.contains(QStringLiteral("one two")));
  EXPECT_TRUE(soft.contains(QStringLiteral("three")));
  EXPECT_EQ(soft.count(QStringLiteral("<p")), 2);

  const QString hard = render(QStringLiteral("one  \ntwo"));
  EXPECT_TRUE(hard.contains(QStringLiteral("one<br/>two")));
}

TEST(ReleaseNotesTest, InlineEmphasisCodeAndStrike) {
  const QString html = render(QStringLiteral(
      "**bold** *italic* _also italic_ ~~gone~~ `--flag`"));
  EXPECT_TRUE(html.contains(QStringLiteral("<b>bold</b>")));
  EXPECT_TRUE(html.contains(QStringLiteral("<i>italic</i>")));
  EXPECT_TRUE(html.contains(QStringLiteral("<i>also italic</i>")));
  EXPECT_TRUE(html.contains(QStringLiteral("<s>gone</s>")));
  EXPECT_TRUE(html.contains(QStringLiteral("<code>--flag</code>")));
}

TEST(ReleaseNotesTest, UnderscoresInsideWordsStayLiteral) {
  const QString html = render(QStringLiteral("set file_name_here, not _this_"));
  EXPECT_TRUE(html.contains(QStringLiteral("file_name_here")));
  EXPECT_TRUE(html.contains(QStringLiteral("<i>this</i>")));
}

TEST(ReleaseNotesTest, EscapedMarkdownStaysLiteral) {
  const QString html = render(QStringLiteral("\\*not italic\\*"));
  EXPECT_TRUE(html.contains(QStringLiteral("*not italic*")));
  EXPECT_FALSE(html.contains(QStringLiteral("<i>")));
}

TEST(ReleaseNotesTest, BulletListRendersItems) {
  const QString html = render(QStringLiteral("- one\n- two"));
  EXPECT_TRUE(html.contains(QStringLiteral("<ul")));
  EXPECT_EQ(html.count(QStringLiteral("<li")), 2);
  EXPECT_TRUE(html.contains(QStringLiteral("one")));
  EXPECT_TRUE(html.contains(QStringLiteral("two")));
  EXPECT_TRUE(html.contains(QStringLiteral("</ul>")));
}

TEST(ReleaseNotesTest, OrderedListKeepsStartNumber) {
  const QString html = render(QStringLiteral("1. one\n2. two"));
  EXPECT_TRUE(html.contains(QStringLiteral("<ol")));
  EXPECT_FALSE(html.contains(QStringLiteral("start=")));

  const QString later = render(QStringLiteral("3. three\n4. four"));
  EXPECT_TRUE(later.contains(QStringLiteral("<ol start=\"3\"")));
}

TEST(ReleaseNotesTest, NestedListStaysInsideItsParentItem) {
  const QString html =
      render(QStringLiteral("- outer\n  - inner\n- outer two"));
  const int outer = html.indexOf(QStringLiteral("outer"));
  const int nested = html.indexOf(QStringLiteral("<ul"), outer);
  const int inner = html.indexOf(QStringLiteral("inner"));
  const int outerTwo = html.indexOf(QStringLiteral("outer two"));
  ASSERT_GE(outer, 0);
  ASSERT_GE(nested, 0);
  ASSERT_GE(inner, 0);
  ASSERT_GE(outerTwo, 0);
  EXPECT_LT(outer, nested);
  EXPECT_LT(nested, inner);
  EXPECT_LT(inner, outerTwo);
  EXPECT_EQ(html.count(QStringLiteral("</ul>")), 2);
}

TEST(ReleaseNotesTest, TaskListItemsGetCheckboxes) {
  const QString html =
      render(QStringLiteral("- [x] done\n- [ ] still to do"));
  EXPECT_TRUE(html.contains(QStringLiteral("\u2611")));
  EXPECT_TRUE(html.contains(QStringLiteral("\u2610")));
  EXPECT_FALSE(html.contains(QStringLiteral("[x]")));
  EXPECT_FALSE(html.contains(QStringLiteral("[ ]")));
}

TEST(ReleaseNotesTest, MarkdownLinksUseAccentColor) {
  const QString html =
      render(QStringLiteral("[Trophy](https://example.com/page)"));
  EXPECT_TRUE(html.contains(
      QStringLiteral("<a href=\"https://example.com/page\"")));
  EXPECT_TRUE(html.contains(QStringLiteral("color:#e94560")));
  EXPECT_TRUE(html.contains(QStringLiteral(">Trophy</a>")));
}

TEST(ReleaseNotesTest, BareAndAutolinksAreClickable) {
  const QString bare = render(QStringLiteral(
      "**Full Changelog**: https://github.com/stevenjwheeler/Trophy/compare/v1...v2."));
  EXPECT_TRUE(bare.contains(QStringLiteral(
      "href=\"https://github.com/stevenjwheeler/Trophy/compare/v1...v2\"")));
  // The sentence period is not swallowed into the link.
  EXPECT_FALSE(bare.contains(QStringLiteral("v2.\"")));

  const QString autoLink =
      render(QStringLiteral("<https://example.com/a>"));
  EXPECT_TRUE(autoLink.contains(
      QStringLiteral("href=\"https://example.com/a\"")));

  const QString www = render(QStringLiteral("see www.example.com/x"));
  EXPECT_TRUE(www.contains(
      QStringLiteral("href=\"https://www.example.com/x\"")));
}

TEST(ReleaseNotesTest, UnsafeLinksBecomePlainText) {
  const QString html =
      render(QStringLiteral("[click](javascript:alert(1)) [file](file:///etc/passwd)"));
  EXPECT_FALSE(html.contains(QStringLiteral("href=\"javascript:")));
  EXPECT_FALSE(html.contains(QStringLiteral("href=\"file:")));
  EXPECT_TRUE(html.contains(QStringLiteral("click")));
  EXPECT_TRUE(html.contains(QStringLiteral("file")));
}

TEST(ReleaseNotesTest, SourceHtmlIsEscaped) {
  const QString html =
      render(QStringLiteral("<script>alert(&\"x\")</script>"));
  EXPECT_TRUE(html.contains(QStringLiteral("&lt;script&gt;")));
  EXPECT_TRUE(html.contains(QStringLiteral("&amp;")));
  EXPECT_FALSE(html.contains(QStringLiteral("<script>")));
}

TEST(ReleaseNotesTest, FencedCodePreservesTextAndEscapesMarkup) {
  const QString html = render(QStringLiteral(
      "```\nif (a < b) {\n  run();\n}\n```"));
  EXPECT_TRUE(html.contains(QStringLiteral("<pre")));
  EXPECT_TRUE(html.contains(QStringLiteral("background-color:")));
  EXPECT_TRUE(html.contains(QStringLiteral("if (a &lt; b) {")));
  EXPECT_TRUE(html.contains(QStringLiteral("&nbsp;&nbsp;  run();")));
  EXPECT_TRUE(html.contains(QStringLiteral("</pre>")));
}

TEST(ReleaseNotesTest, BlockquotesAndRulesRender) {
  const QString quote =
      render(QStringLiteral("> quoted **text**"));
  EXPECT_TRUE(quote.contains(QStringLiteral("<blockquote")));
  EXPECT_TRUE(quote.contains(QStringLiteral("<b>text</b>")));

  const QString rule = render(QStringLiteral("---"));
  EXPECT_TRUE(rule.contains(QStringLiteral("<hr")));
}

TEST(ReleaseNotesTest, ImagesKeepAltTextButNeverEmbed) {
  const QString html =
      render(QStringLiteral("![a screenshot](https://example.com/x.png)"));
  EXPECT_TRUE(html.contains(QStringLiteral("a screenshot")));
  EXPECT_FALSE(html.contains(QStringLiteral("<img")));
  EXPECT_FALSE(html.contains(QStringLiteral("x.png")));
}

TEST(ReleaseNotesTest, CrLfBodiesRenderLikeLf) {
  const QString html = render(QStringLiteral("## A\r\n\r\n- b\r\n- c"));
  EXPECT_TRUE(html.contains(QStringLiteral("<h2")));
  EXPECT_EQ(html.count(QStringLiteral("<li")), 2);
}

TEST(ReleaseNotesTest, RealGitHubReleaseBodyRenders) {
  // The exact shape GitHub generated for Trophy v0.10.1.
  const QString body = QStringLiteral(
      "## What's Changed\r\n"
      "* Trophy will now notify when an update is completed from 0.10.1 onwards.\r\n"
      "* GNOME integration has been refined and tested, and is working end-to-end.\r\n"
      "\r\n"
      "\r\n"
      "**Full Changelog**: https://github.com/stevenjwheeler/Trophy/compare/v0.9.8...v0.10.1");

  const QString html = render(body);
  EXPECT_TRUE(html.contains(QStringLiteral("<h2")));
  EXPECT_EQ(html.count(QStringLiteral("<li")), 2);
  EXPECT_TRUE(html.contains(QStringLiteral(
      "<a href=\"https://github.com/stevenjwheeler/Trophy/compare/v0.9.8...v0.10.1\"")));
  EXPECT_TRUE(html.contains(QStringLiteral("<b>Full Changelog</b>")));
  EXPECT_FALSE(html.contains(QStringLiteral("##")));
  EXPECT_FALSE(html.contains(QStringLiteral("**")));
}

TEST(ReleaseNotesTest, NonAsciiAndEmojiPassThrough) {
  const QString html =
      render(QStringLiteral("## Ünïcode 🎮\n\n- ✓ done"));
  EXPECT_TRUE(html.contains(QStringLiteral("Ünïcode 🎮")));
  EXPECT_TRUE(html.contains(QStringLiteral("✓ done")));
}
