/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "HardwareInfo.h"

#include <QDebug>
#include <QDir>
#include <QFile>

#include "encoding/EncoderFactory.h"

namespace TrophyBackend {

HardwareInfo::HardwareInfo(QObject *parent) : QObject(parent) {
  gatherGpus();
  gatherEncoders();
}

QString HardwareInfo::marketingNameForChip(const QString &chipName) {
  const int open = chipName.indexOf('[');
  const int close = open >= 0 ? chipName.indexOf(']', open + 1) : -1;
  if (open >= 0 && close > open + 1) {
    return chipName.mid(open + 1, close - open - 1).trimmed();
  }
  return chipName;
}

QString HardwareInfo::vendorNameForId(quint64 vendorId) {  switch (vendorId & 0xFFFF) {
  case 0x10DE:
    return "NVIDIA";
  case 0x1002:
    return "AMD";
  case 0x8086:
    return "Intel";
  default:
    break;
  }
  return QString("Unknown (0x%1)")
      .arg((uint)(vendorId & 0xFFFF), 4, 16, QChar('0'));
}

QString HardwareInfo::deviceNameForIds(quint64 vendorId,
                                       const QString &deviceHex,
                                       const QString &dbPathOverride) {
  // pci.ids format: "vvvv  Vendor Name" lines, then tab-indented
  // "dddd  Device Name" lines until the next vendor.
  QString dbPath = dbPathOverride;
  if (dbPath.isEmpty()) {
    const QStringList candidates = {
        "/usr/share/hwdata/pci.ids",
        "/usr/share/misc/pci.ids",
    };
    for (const QString &c : candidates) {
      if (QFile::exists(c)) {
        dbPath = c;
        break;
      }
    }
  }
  if (dbPath.isEmpty())
    return QString();

  bool ok = false;
  const uint vendor = (uint)(vendorId & 0xFFFF);
  const uint device = deviceHex.trimmed()
                          .remove("0x", Qt::CaseInsensitive)
                          .toUInt(&ok, 16);
  if (!ok)
    return QString();

  QFile f(dbPath);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return QString();

  const QString vendorKey =
      QString("%1").arg(vendor, 4, 16, QChar('0')).toLower();
  const QString deviceKey =
      QString("%1").arg(device, 4, 16, QChar('0')).toLower();
  bool inVendor = false;
  while (!f.atEnd()) {
    const QByteArray raw = f.readLine();
    if (raw.isEmpty() || raw.startsWith('#'))
      continue;
    if (raw[0] != '\t' && raw[0] != ' ') {
      // Vendor line: "vvvv  Name".
      inVendor = raw.left(4).toLower() == vendorKey &&
                 (raw.size() <= 4 || raw[4] == ' ' || raw[4] == '\t');
    } else if (inVendor && raw[0] == '\t' && raw.size() > 1 &&
               raw[1] != '\t' && raw[1] != ' ') {
      // Device line: "\tdddd  Name" (two-tab lines are subdevices).
      if (raw.mid(1, 4).toLower() == deviceKey) {
        QString name = QString::fromLocal8Bit(raw.mid(5)).trimmed();
        // pci.ids device names read like "AD107 [GeForce RTX 4060]".
        return name;
      }
    }
  }
  return QString();
}

static QString readSysFile(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return QString();
  return QString::fromLocal8Bit(f.readAll()).trimmed();
}

void HardwareInfo::gatherGpus() {
  // Enumerate $TROPHY_DRM_SYSFS/card* (default /sys/class/drm) — no root,
  // no daemons. Entries without a device/vendor file are not PCI GPUs
  // (e.g. simpledrm/efifb framebuffer leftovers) and are skipped: they
  // have no encoder to offer and only clutter the UI.
  const QByteArray sysfsEnv = qgetenv("TROPHY_DRM_SYSFS");
  const QString sysfsRoot =
      sysfsEnv.isEmpty() ? "/sys/class/drm" : QString::fromLocal8Bit(sysfsEnv);
  const QDir drmDir(sysfsRoot);
  const QStringList cards =
      drmDir.entryList({"card*"}, QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &card : cards) {
    const QString base = sysfsRoot + "/" + card;
    bool ok = false;
    const quint64 vendor =
        readSysFile(base + "/device/vendor").toULongLong(&ok, 16);
    if (!ok)
      continue; // no PCI vendor: framebuffer leftover, not a GPU
    const QString deviceId = readSysFile(base + "/device/device");
    const QString vendorName = vendorNameForId(vendor);
    if (vendorName == "NVIDIA")
      m_hasNvidia = true;
    else if (vendorName == "AMD")
      m_hasAmd = true;
    else if (vendorName == "Intel")
      m_hasIntel = true;

    QString desc;
    // pci.ids device names read like "AD107 [GeForce RTX 4060]" — the
    // bracketed part is the marketing name users recognize, so prefer it
    // over the chip codename, vendor prefix and driver suffix.
    const QString chipName = deviceNameForIds(vendor, deviceId);
    const QString friendly = marketingNameForChip(chipName);
    if (!friendly.isEmpty()) {
      desc = friendly;
    } else {
      desc = vendorName;
      const QString rawId =
          deviceId.trimmed().remove("0x", Qt::CaseInsensitive);
      if (!rawId.isEmpty())
        desc += " 0x" + rawId;
    }
    m_gpuDescriptions.append(desc.trimmed());
  }

  // DMA-BUF zero-copy needs libdrm at build time and a DRM device at run
  // time; the capture path additionally requires PipeWire (always true).
#ifdef HAVE_LIBDRM
  m_dmaBufAvailable = !m_gpuDescriptions.isEmpty();
#else
  m_dmaBufAvailable = false;
#endif
  if (m_gpuDescriptions.isEmpty())
    qDebug() << "HardwareInfo: no DRM GPUs found (headless or no /dev/dri)";
}

void HardwareInfo::gatherEncoders() {
  // NVENC strictly needs an NVIDIA GPU; VA-API is meaningless without any
  // GPU at all. The factory probes still run as ground truth.
  m_nvencAvailable = m_hasNvidia &&
                     EncoderFactory::isAvailable(
                         IEncoder::EncoderType::NVENC_H264);
  m_vaapiAvailable =
      (m_hasAmd || m_hasIntel || m_hasNvidia) &&
      (EncoderFactory::isAvailable(IEncoder::EncoderType::VAAPI_H264) ||
       EncoderFactory::isAvailable(IEncoder::EncoderType::VAAPI_HEVC) ||
       EncoderFactory::isAvailable(IEncoder::EncoderType::VAAPI_AV1));

  if (m_nvencAvailable) {
    m_recommendedName = "nvenc_h264";
    m_recommendedReason =
        "Dedicated NVIDIA encoding chip — minimal FPS impact.";
  } else if (m_vaapiAvailable) {
    m_recommendedName = "vaapi_h264";
    m_recommendedReason =
        "Built-in AMD/Intel media engine — small FPS impact.";
  } else {
    m_recommendedName = "x264";
    m_recommendedReason =
        "Software encoding on the CPU — works everywhere, costs some FPS.";
  }
}

QString HardwareInfo::recommendedEncoderDisplayName() const {
  for (const auto &enc : EncoderFactory::detectAvailableEncoders()) {
    if (enc.name == m_recommendedName) {
      return enc.description;
    }
  }
  return m_recommendedName;
}

} // namespace TrophyBackend
