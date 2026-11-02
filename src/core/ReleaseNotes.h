/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QColor>
#include <QString>

namespace TrophyBackend {

/**
 * @brief Render a GitHub release body into themed Qt rich text.
 *
 * Release bodies are GitHub-flavoured Markdown. The updater dialog
 * receives the raw body from the API, so this converts the subset
 * that release notes actually use (ATX headings, paragraphs, nested
 * bullet/numbered lists, task items, block quotes, fenced code,
 * horizontal rules, and inline emphasis/code/links) into the HTML
 * dialect supported by QML's Text.RichText.
 *
 * All source text is HTML-escaped before our own tags are added, and
 * links are restricted to http(s) and mailto so a release body cannot
 * inject markup, local-file targets, or script URLs. Remote images are
 * reduced to their alt text; the dialog never fetches media.
 *
 * @param markdown    Raw release body (CRLF or LF).
 * @param accentColor Colour for top-level headings and links.
 * @param cardColor   Background behind the notes; used to derive a
 *                    subtle code-block fill.
 */
QString releaseNotesHtml(const QString &markdown, const QColor &accentColor,
                         const QColor &cardColor);

} // namespace TrophyBackend
