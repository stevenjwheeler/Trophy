/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ReleaseNotes.h"

#include <QRegularExpression>
#include <QStringList>
#include <QVector>

namespace TrophyBackend {
namespace {

// GitHub caps release bodies well below this; the guard keeps a
// pathological body from turning a UI refresh into a long parse.
constexpr int kMaxNotesChars = 128 * 1024;

const QRegularExpression kHeadingRe(
    QStringLiteral("^ {0,3}(#{1,6})(?:[ \\t]+(.*))?$"));
const QRegularExpression kFenceRe(
    QStringLiteral("^ {0,3}(`{3,}|~{3,})[ \\t]*([^`]*)$"));
const QRegularExpression kRuleRe(QStringLiteral(
    "^ {0,3}((\\*[ \\t]*){3,}|(-[ \\t]*){3,}|(_[ \\t]*){3,})$"));
const QRegularExpression kQuoteRe(QStringLiteral("^ {0,3}>[ ]?(.*)$"));
const QRegularExpression kListRe(QStringLiteral(
    "^([ \\t]*)([-+*]|\\d{1,9}[.)])[ \\t]+(.*)$"));
const QRegularExpression kTaskRe(
    QStringLiteral("^\\[([ xX])\\][ \\t]+(.*)$"));

QString escapeHtml(const QString &text) {
  QString out;
  out.reserve(text.size() + 16);
  for (const QChar ch : text) {
    switch (ch.unicode()) {
    case '&':
      out += QStringLiteral("&amp;");
      break;
    case '<':
      out += QStringLiteral("&lt;");
      break;
    case '>':
      out += QStringLiteral("&gt;");
      break;
    case '"':
      out += QStringLiteral("&quot;");
      break;
    default:
      out += ch;
      break;
    }
  }
  return out;
}

bool isSafeLinkTarget(const QString &target) {
  const QString lower = target.trimmed().toLower();
  return lower.startsWith(QStringLiteral("https://")) ||
         lower.startsWith(QStringLiteral("http://")) ||
         lower.startsWith(QStringLiteral("mailto:"));
}

QString linkHtml(const QString &labelHtml, const QString &target,
                 const QString &accentName) {
  const QString trimmed = target.trimmed();
  if (!isSafeLinkTarget(trimmed))
    return labelHtml;
  return QStringLiteral("<a href=\"%1\" style=\"color:%2\">%3</a>")
      .arg(escapeHtml(trimmed), accentName, labelHtml);
}

bool isWordChar(const QChar ch) {
  return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

int findClosingBracket(const QString &text, int open) {
  int depth = 0;
  for (int i = open; i < text.size(); ++i) {
    const QChar ch = text.at(i);
    if (ch == QLatin1Char('\\')) {
      ++i;
      continue;
    }
    if (ch == QLatin1Char('[')) {
      ++depth;
    } else if (ch == QLatin1Char(']')) {
      if (--depth == 0)
        return i;
    }
  }
  return -1;
}

int findClosingParen(const QString &text, int from) {
  int depth = 0;
  for (int i = from; i < text.size(); ++i) {
    const QChar ch = text.at(i);
    if (ch == QLatin1Char('\\')) {
      ++i;
      continue;
    }
    if (ch == QLatin1Char('(')) {
      ++depth;
    } else if (ch == QLatin1Char(')')) {
      if (depth == 0)
        return i;
      --depth;
    }
  }
  return -1;
}

// Bare URLs end at whitespace, but sentence punctuation and an
// unbalanced closing paren belong to the surrounding text.
void trimUrlTail(QString *url, int *end) {
  while (!url->isEmpty()) {
    const QChar last = url->back();
    if (QStringLiteral(".,;:!?").contains(last)) {
      url->chop(1);
      --*end;
      continue;
    }
    if (last == QLatin1Char(')') &&
        url->count(QLatin1Char(')')) > url->count(QLatin1Char('('))) {
      url->chop(1);
      --*end;
      continue;
    }
    break;
  }
}

QString inlineHtml(const QString &text, const QString &accentName,
                   bool allowLinks);

QString emphasisHtml(const QString &text, int *index, const QChar marker,
                     const QString &tag, const QString &accentName,
                     bool allowLinks) {
  const int start = *index;
  const int n = text.size();

  // A lone underscore inside a word (file_name) is not emphasis.
  if (marker == QLatin1Char('_') && start > 0 &&
      isWordChar(text.at(start - 1))) {
    return QString();
  }

  int close = text.indexOf(marker, start + 1);
  while (close > start + 1) {
    if (marker != QLatin1Char('_') || close + 1 >= n ||
        !isWordChar(text.at(close + 1))) {
      break;
    }
    close = text.indexOf(marker, close + 1);
  }
  if (close <= start + 1) {
    return QString();
  }

  const QString inner = text.mid(start + 1, close - start - 1);
  *index = close + 1;
  return QStringLiteral("<%1>%2</%1>")
      .arg(tag, inlineHtml(inner, accentName, allowLinks));
}

QString inlineHtml(const QString &text, const QString &accentName,
                   bool allowLinks) {
  QString out;
  out.reserve(text.size() + 32);
  const int n = text.size();
  int i = 0;

  while (i < n) {
    const QChar c = text.at(i);

    if (c == QLatin1Char('\n')) {
      const bool hard = out.endsWith(QStringLiteral("  "));
      while (out.endsWith(QLatin1Char(' ')))
        out.chop(1);
      out += hard ? QStringLiteral("<br/>") : QStringLiteral(" ");
      ++i;
      continue;
    }

    if (c == QLatin1Char('\\') && i + 1 < n &&
        text.at(i + 1) == QLatin1Char('\n')) {
      out += QStringLiteral("<br/>");
      i += 2;
      continue;
    }

    if (c == QLatin1Char('\\') && i + 1 < n &&
        !text.at(i + 1).isLetterOrNumber()) {
      out += escapeHtml(QString(text.at(i + 1)));
      i += 2;
      continue;
    }

    if (c == QLatin1Char('`')) {
      int run = 1;
      while (i + run < n && text.at(i + run) == QLatin1Char('`'))
        ++run;
      const QString fence(run, QLatin1Char('`'));
      int close = i + run;
      while ((close = text.indexOf(fence, close)) >= 0) {
        if (close + run >= n || text.at(close + run) != QLatin1Char('`'))
          break;
        close += run;
      }
      if (close > i + run) {
        QString code = text.mid(i + run, close - (i + run));
        if (code.size() >= 2 && code.startsWith(QLatin1Char(' ')) &&
            code.endsWith(QLatin1Char(' ')) && !code.trimmed().isEmpty()) {
          code = code.mid(1, code.size() - 2);
        }
        out += QStringLiteral("<code>") + escapeHtml(code) +
               QStringLiteral("</code>");
        i = close + run;
      } else {
        out += QString(run, QLatin1Char('`'));
        i += run;
      }
      continue;
    }

    if (c == QLatin1Char('!') && i + 1 < n &&
        text.at(i + 1) == QLatin1Char('[') && allowLinks) {
      const int labelEnd = findClosingBracket(text, i + 1);
      if (labelEnd > 0 && labelEnd + 1 < n &&
          text.at(labelEnd + 1) == QLatin1Char('(')) {
        const int urlEnd = findClosingParen(text, labelEnd + 2);
        if (urlEnd > 0) {
          // Never fetch remote media: keep the alt text, drop the image.
          out += inlineHtml(text.mid(i + 2, labelEnd - (i + 2)),
                            accentName, false);
          i = urlEnd + 1;
          continue;
        }
      }
    }

    if (c == QLatin1Char('[') && allowLinks) {
      const int labelEnd = findClosingBracket(text, i);
      if (labelEnd > 0 && labelEnd + 1 < n &&
          text.at(labelEnd + 1) == QLatin1Char('(')) {
        const int urlEnd = findClosingParen(text, labelEnd + 2);
        if (urlEnd > 0) {
          QString target =
              text.mid(labelEnd + 2, urlEnd - (labelEnd + 2)).trimmed();
          // A Markdown link may carry a "title" after the URL.
          int space = 0;
          while (space < target.size() && !target.at(space).isSpace())
            ++space;
          if (space > 0 && space < target.size())
            target = target.left(space);
          if (target.startsWith(QLatin1Char('<')) &&
              target.endsWith(QLatin1Char('>'))) {
            target = target.mid(1, target.size() - 2);
          }
          const QString label =
              text.mid(i + 1, labelEnd - (i + 1));
          out += linkHtml(inlineHtml(label, accentName, false), target,
                          accentName);
          i = urlEnd + 1;
          continue;
        }
      }
    }

    if (c == QLatin1Char('<') && allowLinks) {
      const int close = text.indexOf(QLatin1Char('>'), i + 1);
      if (close > i + 1) {
        const QString target = text.mid(i + 1, close - (i + 1));
        QString href;
        if (isSafeLinkTarget(target)) {
          href = target;
        } else if (!target.contains(QLatin1Char(' ')) &&
                   target.contains(QLatin1Char('@')) &&
                   target.contains(QLatin1Char('.')) &&
                   !target.contains(QLatin1Char('<'))) {
          href = QStringLiteral("mailto:") + target;
        }
        if (!href.isEmpty()) {
          out += linkHtml(escapeHtml(target), href, accentName);
          i = close + 1;
          continue;
        }
      }
      out += QStringLiteral("&lt;");
      ++i;
      continue;
    }

    if (c == QLatin1Char('<')) {
      out += QStringLiteral("&lt;");
      ++i;
      continue;
    }

    if (c == QLatin1Char('&')) {
      out += QStringLiteral("&amp;");
      ++i;
      continue;
    }

    if (c == QLatin1Char('>')) {
      out += QStringLiteral("&gt;");
      ++i;
      continue;
    }

    if (c == QLatin1Char('"')) {
      out += QStringLiteral("&quot;");
      ++i;
      continue;
    }

    if (allowLinks && (c == QLatin1Char('w') || c == QLatin1Char('W')) &&
        text.mid(i, 4).compare(QStringLiteral("www."),
                               Qt::CaseInsensitive) == 0) {
      const bool boundary =
          i == 0 || text.at(i - 1).isSpace() ||
          QStringLiteral("([<\"'").contains(text.at(i - 1));
      if (boundary) {
        int end = i;
        while (end < n && !text.at(end).isSpace() &&
               !QStringLiteral("<>\"'").contains(text.at(end))) {
          ++end;
        }
        QString url = text.mid(i, end - i);
        trimUrlTail(&url, &end);
        if (url.size() > 4) {
          out += linkHtml(escapeHtml(url),
                          QStringLiteral("https://") + url, accentName);
          i = end;
          continue;
        }
      }
    }

    if (allowLinks && (c == QLatin1Char('h') || c == QLatin1Char('H')) &&
        (text.mid(i, 7).compare(QStringLiteral("http://"),
                                Qt::CaseInsensitive) == 0 ||
         text.mid(i, 8).compare(QStringLiteral("https://"),
                                Qt::CaseInsensitive) == 0)) {
      bool boundary = i == 0;
      if (!boundary) {
        const QChar prev = text.at(i - 1);
        boundary = prev.isSpace() || QStringLiteral("([<:\"'").contains(prev);
      }
      if (boundary) {
        int end = i;
        while (end < n && !text.at(end).isSpace() &&
               !QStringLiteral("<>\"'").contains(text.at(end))) {
          ++end;
        }
        QString url = text.mid(i, end - i);
        trimUrlTail(&url, &end);
        if (url.size() > 8) {
          out += linkHtml(escapeHtml(url), url, accentName);
          i = end;
          continue;
        }
      }
    }

    if (c == QLatin1Char('*') || c == QLatin1Char('_')) {
      const bool strong = i + 1 < n && text.at(i + 1) == c;
      if (strong) {
        const QString marker = QString(c) + QString(c);
        const int close = text.indexOf(marker, i + 2);
        if (close > i + 2) {
          out += QStringLiteral("<b>") +
                 inlineHtml(text.mid(i + 2, close - (i + 2)), accentName,
                            allowLinks) +
                 QStringLiteral("</b>");
          i = close + 2;
          continue;
        }
      }
      const QString emphasis =
          emphasisHtml(text, &i, c, QStringLiteral("i"), accentName, allowLinks);
      if (!emphasis.isEmpty()) {
        out += emphasis;
        continue;
      }
    }

    if (c == QLatin1Char('~') && i + 1 < n &&
        text.at(i + 1) == QLatin1Char('~')) {
      const int close = text.indexOf(QStringLiteral("~~"), i + 2);
      if (close > i + 2) {
        out += QStringLiteral("<s>") +
               inlineHtml(text.mid(i + 2, close - (i + 2)), accentName,
                          allowLinks) +
               QStringLiteral("</s>");
        i = close + 2;
        continue;
      }
    }

    out += escapeHtml(QString(c));
    ++i;
  }

  return out;
}

int indentColumns(const QString &whitespace) {
  int columns = 0;
  for (const QChar ch : whitespace)
    columns += ch == QLatin1Char('\t') ? 4 : 1;
  return columns;
}

QString leadingWhitespace(const QString &line) {
  int i = 0;
  while (i < line.size() &&
         (line.at(i) == QLatin1Char(' ') || line.at(i) == QLatin1Char('\t'))) {
    ++i;
  }
  return line.left(i);
}

bool isBlockStart(const QString &line) {
  return kHeadingRe.match(line).hasMatch() ||
         kFenceRe.match(line).hasMatch() ||
         kRuleRe.match(line).hasMatch() ||
         kQuoteRe.match(line).hasMatch() ||
         kListRe.match(line).hasMatch();
}

struct ListItem {
  int indent = 0;
  int depth = 0;
  bool ordered = false;
  int number = 1;
  QString text;
};

QString buildListHtml(const QVector<ListItem> &items, int *index, int depth,
                      const QString &accentName);

QString listItemHtml(const ListItem &item, const QString &accentName) {
  QString text = item.text;
  QString prefix;
  const auto task = kTaskRe.match(text);
  if (task.hasMatch()) {
    const bool checked = task.captured(1).compare(QStringLiteral(" ")) != 0;
    prefix = checked
                 ? QStringLiteral("<span style=\"color:%1\">\u2611</span> ")
                       .arg(accentName)
                 : QStringLiteral("\u2610 ");
    text = task.captured(2);
  }
  return prefix + inlineHtml(text, accentName, true);
}

QString buildListHtml(const QVector<ListItem> &items, int *index, int depth,
                      const QString &accentName) {
  if (*index >= items.size() || items[*index].depth != depth)
    return QString();

  const bool ordered = items[*index].ordered;
  QString out = ordered ? QStringLiteral("<ol") : QStringLiteral("<ul");
  if (ordered && items[*index].number != 1) {
    out += QStringLiteral(" start=\"%1\"").arg(items[*index].number);
  }
  out += QStringLiteral(
      " style=\"margin-top:2px; margin-bottom:8px; margin-left:10px\">");

  while (*index < items.size()) {
    const ListItem &item = items[*index];
    if (item.depth != depth || item.ordered != ordered)
      break;

    out += QStringLiteral("<li style=\"margin-bottom:3px\">");
    out += listItemHtml(item, accentName);
    ++*index;

    if (*index < items.size() && items[*index].depth > depth) {
      out += buildListHtml(items, index, items[*index].depth, accentName);
    }
    out += QStringLiteral("</li>");
  }

  out += ordered ? QStringLiteral("</ol>") : QStringLiteral("</ul>");
  return out;
}

QString renderBlocks(const QStringList &lines, const QString &accentName,
                     const QString &codeBackground,
                     const QString &dividerColor) {
  QString out;
  int i = 0;
  bool first = true;
  const int total = lines.size();

  const auto headingStyle = [&](int level, bool isFirst) {
    int size = 14;
    int top = 16;
    if (level == 1) {
      size = 24;
      top = 20;
    } else if (level == 2) {
      size = 20;
      top = 20;
    } else if (level == 3) {
      size = 16;
    }
    if (isFirst)
      top = 2;
    QString style = QStringLiteral(
        "font-size:%1px; font-weight:bold; margin-top:%2px; "
        "margin-bottom:%3px")
                        .arg(size)
                        .arg(top)
                        .arg(level <= 2 ? 8 : 6);
    if (level <= 2)
      style += QStringLiteral("; color:") + accentName;
    return style;
  };

  while (i < total) {
    const QString &line = lines.at(i);
    if (line.trimmed().isEmpty()) {
      ++i;
      continue;
    }

    const auto fence = kFenceRe.match(line);
    if (fence.hasMatch()) {
      const QChar fenceChar = fence.captured(1).at(0);
      const int fenceLength = fence.captured(1).size();
      QStringList code;
      ++i;
      while (i < total) {
        const QString trimmed = lines.at(i).trimmed();
        bool closes = trimmed.size() >= fenceLength;
        if (closes) {
          for (const QChar ch : trimmed) {
            if (ch != fenceChar) {
              closes = false;
              break;
            }
          }
        }
        ++i;
        if (closes)
          break;
        code.append(lines.at(i - 1));
      }
      QString body;
      QStringList escaped;
      escaped.reserve(code.size());
      for (const QString &codeLine : code)
        escaped.append(QStringLiteral("&nbsp;&nbsp;") + escapeHtml(codeLine));
      body = escaped.join(QLatin1Char('\n'));
      out += QStringLiteral(
                 "<pre style=\"margin-top:%1px; margin-bottom:8px; "
                 "background-color:%2\">")
                 .arg(first ? 2 : 8)
                 .arg(codeBackground);
      out += QStringLiteral("&nbsp;\n") + body + QStringLiteral("\n&nbsp;</pre>");
      first = false;
      continue;
    }

    const auto heading = kHeadingRe.match(line);
    if (heading.hasMatch()) {
      QString content = heading.captured(2);
      content.remove(QRegularExpression(QStringLiteral("[ \\t]+#+[ \\t]*$")));
      const int level = heading.captured(1).size();
      if (!content.trimmed().isEmpty()) {
        out += QStringLiteral("<h%1 style=\"%2\">%3</h%1>")
                   .arg(level)
                   .arg(headingStyle(level, first),
                        inlineHtml(content.trimmed(), accentName, true));
        first = false;
      }
      ++i;
      continue;
    }

    if (kRuleRe.match(line).hasMatch()) {
      out += QStringLiteral(
          "<hr style=\"margin-top:8px; margin-bottom:8px; height:1px; "
          "background-color:%1\">")
                 .arg(dividerColor);
      first = false;
      ++i;
      continue;
    }

    const auto quote = kQuoteRe.match(line);
    if (quote.hasMatch()) {
      QStringList inner;
      while (i < total) {
        const auto nested = kQuoteRe.match(lines.at(i));
        if (!nested.hasMatch())
          break;
        inner.append(nested.captured(1));
        ++i;
      }
      out += QStringLiteral(
          "<blockquote style=\"margin-top:%1px; margin-bottom:8px; "
          "margin-left:10px; font-style:italic\">")
                 .arg(first ? 2 : 8);
      out += renderBlocks(inner, accentName, codeBackground, dividerColor);
      out += QStringLiteral("</blockquote>");
      first = false;
      continue;
    }

    if (kListRe.match(line).hasMatch()) {
      QVector<ListItem> items;
      QVector<int> indents;
      while (i < total) {
        const QString &itemLine = lines.at(i);
        if (itemLine.trimmed().isEmpty()) {
          int next = i + 1;
          while (next < total && lines.at(next).trimmed().isEmpty())
            ++next;
          if (next < total && kListRe.match(lines.at(next)).hasMatch()) {
            i = next;
            continue;
          }
          break;
        }
        const auto marker = kListRe.match(itemLine);
        if (!marker.hasMatch())
          break;

        ListItem item;
        item.indent = indentColumns(marker.captured(1));
        const QString bullet = marker.captured(2);
        item.ordered = bullet.at(0).isDigit();
        if (item.ordered) {
          item.number =
              bullet.left(bullet.size() - 1).toInt();
        }
        item.text = marker.captured(3);

        int next = i + 1;
        while (next < total) {
          const QString &continuation = lines.at(next);
          if (continuation.trimmed().isEmpty())
            break;
          if (kListRe.match(continuation).hasMatch())
            break;
          if (indentColumns(leadingWhitespace(continuation)) <= item.indent)
            break;
          item.text += QLatin1Char(' ') + continuation.trimmed();
          ++next;
        }

        while (!indents.isEmpty() && item.indent < indents.last())
          indents.removeLast();
        if (indents.isEmpty() || item.indent > indents.last())
          indents.append(item.indent);
        item.depth = indents.size() - 1;
        items.append(item);
        i = next;
      }

      if (!items.isEmpty()) {
        int index = 0;
        while (index < items.size())
          out += buildListHtml(items, &index, items.at(index).depth,
                               accentName);
        first = false;
      }
      continue;
    }

    QStringList paragraph;
    while (i < total) {
      const QString &paragraphLine = lines.at(i);
      if (paragraphLine.trimmed().isEmpty())
        break;
      if (isBlockStart(paragraphLine))
        break;
      QString stripped = paragraphLine;
      while (stripped.startsWith(QLatin1Char(' ')))
        stripped.remove(0, 1);
      paragraph.append(stripped);
      ++i;
    }
    if (!paragraph.isEmpty()) {
      out += QStringLiteral(
                 "<p style=\"margin-top:%1px; margin-bottom:8px\">")
                 .arg(first ? 0 : 2);
      out += inlineHtml(paragraph.join(QLatin1Char('\n')), accentName, true);
      out += QStringLiteral("</p>");
      first = false;
    }
  }

  return out;
}

} // namespace

QString releaseNotesHtml(const QString &markdown, const QColor &accentColor,
                         const QColor &cardColor) {
  QString source = markdown;
  if (source.size() > kMaxNotesChars)
    source.truncate(kMaxNotesChars);
  source.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  source.replace(QLatin1Char('\r'), QLatin1Char('\n'));

  QColor codeBackground;
  constexpr qreal kCodeMix = 0.10;
  codeBackground = QColor::fromRgbF(
      cardColor.redF() * (1.0 - kCodeMix) + kCodeMix,
      cardColor.greenF() * (1.0 - kCodeMix) + kCodeMix,
      cardColor.blueF() * (1.0 - kCodeMix) + kCodeMix);

  QColor divider;
  constexpr qreal kDividerMix = 0.16;
  divider = QColor::fromRgbF(
      cardColor.redF() * (1.0 - kDividerMix) + kDividerMix,
      cardColor.greenF() * (1.0 - kDividerMix) + kDividerMix,
      cardColor.blueF() * (1.0 - kDividerMix) + kDividerMix);

  const QString accentName =
      accentColor.isValid() ? accentColor.name(QColor::HexRgb)
                            : QStringLiteral("#e94560");

  return renderBlocks(source.split(QLatin1Char('\n')), accentName,
                      codeBackground.name(QColor::HexRgb),
                      divider.name(QColor::HexRgb));
}

} // namespace TrophyBackend
