/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "VulkanLayer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSaveFile>

#include "detection/ShippedLibDirs.h"
#include "detection/PersistentReporterLib.h"

namespace TrophyBackend {

namespace {
constexpr const char *kLayerName = "VK_LAYER_TROPHY_detection";
constexpr const char *kFileName = "trophy_layer.json";
constexpr const char *kLibName = "libtrophy_layer.so";
} // namespace

QString VulkanLayer::destManifestPath() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         "/vulkan/implicit_layer.d/" + kFileName;
}

QString VulkanLayer::sourceManifestPath() {
  // Dev build tree: manifest + lib side by side next to the binaries.
  const QString appDir = QCoreApplication::applicationDirPath();
  const QString dev = appDir + "/vulkan_layer/" + kFileName;
  if (QFile::exists(dev))
    return dev;

  // Our own install prefix (AppImage included): the bundled template
  // matches the running code, so it wins over any copy an older
  // install left in the shared datadir.
  const QString bundled = prefixFromBinaryDir(appDir) +
                          "/share/trophy/vulkan_layer/" + kFileName;
  if (QFile::exists(bundled))
    return bundled;

  // Installed datadir.
  const QString installed = QStandardPaths::locate(
      QStandardPaths::GenericDataLocation,
      "trophy/vulkan_layer/" + QString(kFileName));
  if (!installed.isEmpty())
    return installed;

  return QString();
}

QString VulkanLayer::libraryPathForSource(const QString &srcManifestPath) {
  if (srcManifestPath.isEmpty())
    return QString();
  // Side-by-side first (dev build tree).
  const QString sibling =
      QFileInfo(srcManifestPath).absolutePath() + "/" + kLibName;
  if (QFile::exists(sibling))
    return sibling;

  // Installed libdirs: derived from the manifest prefix and our own
  // executable, so ~/.local and other custom prefixes resolve (a hardcoded
  // /usr list silently broke every non-system install). Pass the manifest's
  // directory — findShippedLib appends the file name itself.
  return findShippedLib(kLibName,
                        {QFileInfo(srcManifestPath).absolutePath()},
                        srcManifestPath);
}

bool VulkanLayer::writeManifest(const QString &srcJsonPath,
                                const QString &libraryPath,
                                const QString &destPath) {
  QFile src(srcJsonPath);
  if (!src.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  QJsonParseError err;
  const QJsonDocument doc =
      QJsonDocument::fromJson(src.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return false;

  QJsonObject root = doc.object();
  const QString durableLib =
      PersistentReporterLib::install(libraryPath, "libtrophy_layer");
  if (durableLib.isEmpty())
    return false;
  QJsonObject layer = root.value("layer").toObject();
  layer["library_path"] = durableLib;
  root["layer"] = layer;

  if (!QDir().mkpath(QFileInfo(destPath).absolutePath()))
    return false;
  QSaveFile dst(destPath);
  if (!dst.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  const QByteArray json = QJsonDocument(root).toJson();
  return dst.write(json) == json.size() && dst.commit();
}

bool VulkanLayer::isRegistered() {
  const QString dest = destManifestPath();
  QFile f(dest);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return false;
  const QString lib =
      doc.object().value("layer").toObject().value("library_path").toString();
  return !lib.isEmpty() && QFile::exists(lib);
}

bool VulkanLayer::ensureRegistered() {
  const QString src = sourceManifestPath();
  const QString shippedLib = libraryPathForSource(src);
  const QString lib = shippedLib.isEmpty()
                          ? QString()
                          : PersistentReporterLib::install(shippedLib,
                                                           "libtrophy_layer");
  if (src.isEmpty() || lib.isEmpty()) {
    qWarning() << "Vulkan layer: shipped manifest/library not found";
    return false;
  }
  // Existence alone is not enough: after an update the installed lib path
  // may have moved while the old manifest still points at a present-but-old
  // copy. Compare the stored path and rewrite on mismatch.
  const QString dest = destManifestPath();
  QFile cur(dest);
  if (cur.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(cur.readAll(), &err);
    if (err.error == QJsonParseError::NoError && doc.isObject()) {
      const QString stored = doc.object()
                                 .value("layer")
                                 .toObject()
                                 .value("library_path")
                                 .toString();
      if (stored == lib && QFile::exists(lib))
        return true; // already correct: no rewrite, no loader churn
    }
  }
  if (!writeManifest(src, lib, dest)) {
    qWarning() << "Vulkan layer: cannot write" << dest;
    return false;
  }
  qInfo() << "Vulkan detection layer registered:" << lib;
  return true;
}

bool VulkanLayer::removeRegistration() {
  const QString dest = destManifestPath();
  if (!QFile::exists(dest))
    return true;
  return QFile::remove(dest);
}

} // namespace TrophyBackend
