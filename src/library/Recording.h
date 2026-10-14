/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDateTime>
#include <QFuture>
#include <QObject>
#include <QString>

namespace TrophyBackend {

/**
 * @brief Represents a single video recording
 *
 * Contains metadata about the recording including file info,
 * game name, duration, and thumbnail path.
 */
class Recording : public QObject {
  Q_OBJECT

  Q_PROPERTY(QString filePath READ filePath CONSTANT)
  Q_PROPERTY(QString fileName READ fileName CONSTANT)
  Q_PROPERTY(QString directory READ directory CONSTANT)
  Q_PROPERTY(QString gameName READ gameName NOTIFY metadataChanged)
  // Produced asynchronously by probeVideoFile: filesystem dates first,
  // then the container tags (trophy-date/time win over creation_time).
  Q_PROPERTY(QDateTime date READ date NOTIFY metadataChanged)
  Q_PROPERTY(qint64 duration READ duration NOTIFY metadataChanged)
  Q_PROPERTY(qint64 size READ size CONSTANT)
  Q_PROPERTY(
      QString thumbnailPath READ thumbnailPath NOTIFY thumbnailPathChanged)
  Q_PROPERTY(int width READ width NOTIFY metadataChanged)
  Q_PROPERTY(int height READ height NOTIFY metadataChanged)
  // Media kind: container tag (trophy-kind) wins, _Replay filename
  // suffix is the fallback for older files. Survives renames.
  Q_PROPERTY(bool isClip READ isClip NOTIFY metadataChanged)
  // Recorder version that wrote the file (trophy-version tag; empty on
  // older files that predate the tag).
  Q_PROPERTY(QString trophyVersion READ trophyVersion NOTIFY metadataChanged)
  // Stream layout from the same probe pass: codec descriptor names
  // ("h264", "aac", ...), measured video frame rate, audio stream
  // count. Empty/zero until the probe lands (or on probe failure).
  Q_PROPERTY(QString videoCodec READ videoCodec NOTIFY metadataChanged)
  Q_PROPERTY(double fps READ fps NOTIFY metadataChanged)
  Q_PROPERTY(int audioTrackCount READ audioTrackCount NOTIFY metadataChanged)
  Q_PROPERTY(QString audioCodec READ audioCodec NOTIFY metadataChanged)
  // Which encoder wrote the pixels, display-ready ("NVENC").
  // Stamped as trophy-encoder at record time; empty on older files
  // that predate the tag (the dialog then shows codec only).
  Q_PROPERTY(QString encoderName READ encoderName NOTIFY metadataChanged)

public:
  explicit Recording(const QString &filePath, QObject *parent = nullptr);
  ~Recording() override;

  QString filePath() const { return m_filePath; }
  QString fileName() const { return m_fileName; }
  QString directory() const { return m_directory; }
  QString gameName() const { return m_gameName; }
  QDateTime date() const { return m_date; }
  qint64 duration() const { return m_duration; } // seconds
  qint64 size() const { return m_size; }         // bytes
  QString thumbnailPath() const { return m_thumbnailPath; }
  int width() const { return m_width; }
  int height() const { return m_height; }
  bool isClip() const { return m_isClip; }
  QString trophyVersion() const { return m_trophyVersion; }
  QString videoCodec() const { return m_videoCodec; }
  double fps() const { return m_fps; }
  int audioTrackCount() const { return m_audioTrackCount; }
  QString audioCodec() const { return m_audioCodec; }
  QString encoderName() const { return m_encoderName; }
  // Canonical trophy-encoder tag ("nvenc_h264", "x264", ...) to the
  // display string the file-info dialog shows. Unknown tags pass
  // through raw rather than hiding behind "Unknown".
  static QString encoderDisplayName(const QString &tag);

  void setThumbnailPath(const QString &path);

  /**
   * @brief Prefer the filename-derived title over container metadata.
   *
   * Set for user-renamed files: the container still carries the title
   * baked in at record time, so without this the background probe would
   * overwrite the new filename-derived game name with the stale tag.
   * Must be called before loadMetadata().
   */
  void setPreferFileNameTitle(bool prefer) { m_preferFileNameTitle = prefer; }

  /**
   * @brief Load metadata from the video file
   */
  void loadMetadata();

signals:
  void thumbnailPathChanged();
  void metadataChanged();

private:
  struct ProbeResult {
    QString gameName;
    QDateTime date;
    qint64 duration = 0;
    int width = 0;
    int height = 0;
    bool isClip = false;
    QString trophyVersion;
    QString videoCodec;
    double fps = 0;
    int audioTrackCount = 0;
    QString audioCodec;
    QString encoderName;
  };
  void parseFileNameForGameName();
  static ProbeResult probeVideoFile(const QString &filePath, ProbeResult result,
                                    bool preferFileNameTitle);
  void applyProbeResult(const ProbeResult &result);

  QString m_filePath;
  QString m_fileName;
  QString m_directory;
  QString m_gameName;
  QDateTime m_date;
  qint64 m_duration = 0;
  qint64 m_size = 0;
  QString m_thumbnailPath;
  int m_width = 0;
  int m_height = 0;
  bool m_preferFileNameTitle = false;
  bool m_isClip = false;
  QString m_trophyVersion;
  QString m_videoCodec;
  double m_fps = 0;
  int m_audioTrackCount = 0;
  QString m_audioCodec;
  QString m_encoderName;
  QFuture<void> m_probeFuture;
  quint64 m_probeGeneration = 0;
};

} // namespace TrophyBackend
