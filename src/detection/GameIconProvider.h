/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QQuickImageProvider>
#include <QCache>
#include <QImage>
#include <QHash>
#include <QMutex>
#include <QStringList>

namespace TrophyBackend {

// Local artwork only. Qt owns this provider and runs requests on its image
// loading thread; no manager/QObject or native game process is consulted.
class GameIconProvider : public QQuickImageProvider {
public:
  GameIconProvider();
  // Explicit roots keep fixtures independent of the user's desktop/Steam data.
  // An empty cacheFile disables disk caching (the production constructor uses
  // Qt's application cache directory, never settings or the game library).
  GameIconProvider(QStringList dataRoots, QStringList iconRoots,
                   QString theme, QStringList steamRoots,
                   QString cacheFile = {});

  QImage requestImage(const QString &id, QSize *size,
                      const QSize &requestedSize) override;

private:
  struct SourceEntry {
    QString path; // empty = cached miss; :/ = bundled fallback
    qint64 modifiedMs = 0;
    qint64 bytes = 0;
    qint64 discoveredMs = 0;
  };
  struct ImageEntry {
    QImage image;
    qint64 loadedMs = 0;
  };
  QImage loadImage(const QString &id, int extent, QString *foundPath) const;
  void loadSourceCache();
  void saveSourceCache() const;
  QStringList m_dataRoots;
  QStringList m_iconRoots;
  QString m_theme;
  QStringList m_steamRoots;
  QString m_cacheFile;
  QString m_cacheFingerprint;
  bool m_sourceCacheLoaded = false;
  QHash<QString, SourceEntry> m_sources;
  QMutex m_cacheMutex;
  QCache<QString, ImageEntry> m_cache{256}; // includes misses; bounded per engine
};

} // namespace TrophyBackend
