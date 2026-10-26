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
#include <QJsonArray>
#include <QStandardPaths>
#include <QSaveFile>

#include "detection/ShippedLibDirs.h"
#include "detection/PersistentReporterLib.h"
#include "detection/ReporterArchitecture.h"
#include "detection/DetectorVersion.h"

namespace TrophyBackend {

namespace {
constexpr const char *kLayerName = "VK_LAYER_TROPHY_detection";
constexpr const char *kFileName = "trophy_layer.json";
constexpr const char *kLibName = "libtrophy_layer.so";

QJsonDocument readManifest(const QString &path) {
  const QFileInfo info(path);
  if (!info.isFile() || info.size() > 1024 * 1024) return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  return file.error() == QFileDevice::NoError && error.error == QJsonParseError::NoError
             ? document : QJsonDocument();
}

QString registeredLibrary(const QString &name) {
  const auto root = readManifest(VulkanLayer::destManifestPath()).object();
  QJsonArray entries = root.value("layers").toArray();
  if (root.value("layer").isObject()) entries.append(root.value("layer"));
  for (const auto &entry : entries) {
    const auto layer = entry.toObject();
    if (layer.value("name").toString() == name)
      return layer.value("library_path").toString();
  }
  return {};
}
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

QString VulkanLayer::libraryPath32ForSource(const QString &srcManifestPath) {
  return ReporterArchitecture::companion32(libraryPathForSource(srcManifestPath));
}

bool VulkanLayer::writeManifest(const QString &srcJsonPath,
                                const QString &libraryPath,
                                const QString &destPath,
                                const QString &libraryPath32) {
  const auto doc = readManifest(srcJsonPath);
  if (!doc.isObject() || !doc.object().value("layer").isObject())
    return false;

  QJsonObject root = doc.object();
  QJsonObject layer = root.value("layer").toObject();
  if (layer.value("name").toString() != kLayerName) return false;
  if (!libraryPath32.isEmpty() &&
      (QFileInfo(libraryPath32).isSymLink() ||
       ReporterArchitecture::readSharedObject(libraryPath, 64).isEmpty() ||
       ReporterArchitecture::readSharedObject(libraryPath32, 32).isEmpty()))
    return false;
  const QString durableLib =
      PersistentReporterLib::install(libraryPath, "libtrophy_layer");
  if (durableLib.isEmpty())
    return false;
  layer["library_path"] = durableLib;
  root.remove("layers");
  if (libraryPath32.isEmpty()) {
    layer.remove("library_arch");
    root["file_format_version"] = "1.0.0"; // Preserve legacy loader compatibility.
    root["layer"] = layer;
  } else {
    root["file_format_version"] = "1.2.1";
    layer["library_arch"] = "64";
    const QString durable32 = PersistentReporterLib::install(libraryPath32, "libtrophy_layer32");
    if (durable32.isEmpty()) return false;
    QJsonObject layer32 = layer;
    layer32["name"] = QString(kLayerName) + "_32";
    layer32["library_path"] = durable32;
    layer32["library_arch"] = "32";
    root.remove("layer");
    root["layers"] = QJsonArray{layer, layer32};
  }
  const QByteArray json = QJsonDocument(root).toJson();
  const QFileInfo destination(destPath);
  if ((destination.exists() || destination.isSymLink()) &&
      (destination.isSymLink() || !destination.isFile() || destination.size() > 1024 * 1024))
    return false;
  QFile current(destPath);
  if (current.open(QIODevice::ReadOnly) && current.readAll() == json)
    return true;

  if (!QDir().mkpath(QFileInfo(destPath).absolutePath()))
    return false;
  QSaveFile dst(destPath);
  if (!dst.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  return dst.write(json) == json.size() && dst.commit();
}

bool VulkanLayer::isRegistered() {
  const QString library = registeredLibrary(kLayerName);
  return !library.isEmpty() && QFileInfo(library).isFile();
}

int VulkanLayer::bundledHelperVersion() {
  return DetectorVersion::reporter(libraryPathForSource(sourceManifestPath()), "vulkan");
}

int VulkanLayer::helperVersion() {
  const QString registered = registeredLibrary(kLayerName);
  return DetectorVersion::reporter(registered.isEmpty()
      ? libraryPathForSource(sourceManifestPath()) : registered, "vulkan");
}

bool VulkanLayer::is32BitRegistered() {
  const QString library = registeredLibrary(QString(kLayerName) + "_32");
  return isRegistered() && !ReporterArchitecture::readSharedObject(library, 32).isEmpty();
}

bool VulkanLayer::ensureRegistered(bool allow32BitMigration) {
  const QString src = sourceManifestPath();
  const QString shippedLib = libraryPathForSource(src);
  if (src.isEmpty() || shippedLib.isEmpty() || !QFileInfo(shippedLib).isFile()) {
    qWarning() << "Vulkan layer: shipped manifest/library not found";
    return false;
  }
  // One atomic document publishes both architecture-filtered entries. Pure
  // status/library lookups never install helpers or grant new enrollment.
  const QString dest = destManifestPath();
  const bool pairedConsent = !registeredLibrary(QString(kLayerName) + "_32").isEmpty();
  const QString companion = allow32BitMigration || pairedConsent ? libraryPath32ForSource(src) : QString();
  if (!writeManifest(src, shippedLib, dest, companion)) {
    qWarning() << "Vulkan layer: cannot write" << dest;
    return false;
  }
  qInfo() << "Vulkan detection layer registered:" << registeredLibrary(kLayerName);
  return true;
}

bool VulkanLayer::removeRegistration() {
  const QString dest = destManifestPath();
  if (!QFile::exists(dest))
    return true;
  return QFile::remove(dest);
}

} // namespace TrophyBackend
