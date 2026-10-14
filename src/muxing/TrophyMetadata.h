/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDateTime>
#include <QMap>
#include <QString>

// Container metadata tags baked into every recording and clip, alongside
// the pre-existing "title", "creation_time" and "trophy-kind" tags.
// Display tools uppercase tag names, so these read as TROPHY-KIND /
// TROPHY-VERSION / TROPHY-DATE / TROPHY-TIME there. Keys stay lowercase
// here to match the existing trophy-kind convention (FFmpeg dict lookup
// is case-insensitive; Matroska uppercases on write).
//
// trophy-date + trophy-time mirror the local-time filename timestamp
// (yyyy-MM-dd + HH-mm-ss), so the library date no longer depends on the
// filename: renames, copies and timestamp-stripping all keep working.
// Very old/tagless files still fall back to creation_time, then the
// filesystem dates. Note MKV preserves arbitrary tags; MP4 may drop
// unknown keys, in which case the same fallbacks apply.

#define TROPHY_META_STRINGIFY(x) #x
#define TROPHY_META_TOSTRING(x) TROPHY_META_STRINGIFY(x)

namespace TrophyBackend {

inline QString trophyVersion() {
#ifdef APP_VERSION
  // add_compile_definitions quotes the value, so the macro expands with
  // literal quote characters (same handling as Application::initialize).
  QString v = QString::fromUtf8(TROPHY_META_TOSTRING(APP_VERSION));
  v.remove(QLatin1Char('"'));
  return v.isEmpty() ? QStringLiteral("unknown") : v;
#else
  return QStringLiteral("0.0.0-unknown");
#endif
}

inline QString trophyDateString(const QDateTime &when) {
  return when.toString(QStringLiteral("yyyy-MM-dd"));
}

inline QString trophyTimeString(const QDateTime &when) {
  return when.toString(QStringLiteral("HH-mm-ss"));
}

// Combine a trophy-date + trophy-time pair back into a QDateTime (local
// time, as written). Invalid when either half is missing or malformed.
inline QDateTime trophyDateTimeFromTags(const QString &dateStr,
                                        const QString &timeStr) {
  if (dateStr.isEmpty() || timeStr.isEmpty())
    return QDateTime();
  const QDateTime dt = QDateTime::fromString(
      dateStr.trimmed() + QStringLiteral("_") + timeStr.trimmed(),
      QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
  if (!dt.isValid())
    return QDateTime();
  return dt;
}

// Version + capture moment only (DVR segments: not recordings, not clips,
// so they carry no trophy-kind; the clipper stamps kind=clip plus a fresh
// save moment when it remuxes).
inline void
stampTrophyVersionDateTime(QMap<QString, QString> &meta,
                           const QDateTime &now = QDateTime::currentDateTime()) {
  meta[QStringLiteral("trophy-version")] = trophyVersion();
  meta[QStringLiteral("trophy-date")] = trophyDateString(now);
  meta[QStringLiteral("trophy-time")] = trophyTimeString(now);
}

// Which encoder wrote the pixels (canonical EncoderFactory name, e.g.
// "nvenc_h264" or "x264"). The container codec ("H.264") cannot tell
// NVENC apart from x264 after the fact; this tag can. Read back by the
// library probe for the player file-info dialog; clips inherit it via
// the clipper's metadata copy. Same MP4 caveat as the other tags.
inline void stampTrophyEncoder(QMap<QString, QString> &meta,
                               const QString &encoderName) {
  if (!encoderName.isEmpty())
    meta[QStringLiteral("trophy-encoder")] = encoderName;
}

// Full stamp for finished files: kind + version + capture moment.
// `now` is local time (matches the filename timestamp); creation_time
// stays a separate UTC ISO string owned by the call sites.
inline void stampTrophyTags(QMap<QString, QString> &meta, const QString &kind,
                            const QDateTime &now = QDateTime::currentDateTime()) {
  meta[QStringLiteral("trophy-kind")] = kind;
  stampTrophyVersionDateTime(meta, now);
}

} // namespace TrophyBackend
