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

#include "detection/ShippedLibDirs.h"

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
  QJsonObject layer = root.value("layer").toObject();
  layer["library_path"] = libraryPath;
  root["layer"] = layer;

  QDir().mkpath(QFileInfo(destPath).absolutePath());
  QFile dst(destPath);
  if (!dst.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    return false;
  dst.write(QJsonDocument(root).toJson());
  return true;
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
  if (isRegistered())
    return true; // already correct: no rewrite, no loader churn
  const QString src = sourceManifestPath();
  const QString lib = libraryPathForSource(src);
  if (src.isEmpty() || lib.isEmpty()) {
    qWarning() << "Vulkan layer: shipped manifest/library not found";
    return false;
  }
  if (!writeManifest(src, lib, destManifestPath())) {
    qWarning() << "Vulkan layer: cannot write" << destManifestPath();
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
