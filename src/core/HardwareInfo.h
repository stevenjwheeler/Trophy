/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace TrophyBackend {

/**
 * @brief Read-only hardware inventory for the setup wizard.
 *
 * Enumerates DRM GPUs (/sys + libdrm, no new dependencies), reports
 * NVENC / VA-API availability via EncoderFactory probes, and DMA-BUF
 * zero-copy readiness. Everything degrades to "unknown/unavailable"
 * headless or driverless — never fatal. Constructing is cheap; results
 * are gathered once and cached.
 */
class HardwareInfo : public QObject {
  Q_OBJECT
  Q_PROPERTY(QStringList gpuDescriptions READ gpuDescriptions CONSTANT)
  Q_PROPERTY(bool hasNvidiaGpu READ hasNvidiaGpu CONSTANT)
  Q_PROPERTY(bool hasAmdGpu READ hasAmdGpu CONSTANT)
  Q_PROPERTY(bool hasIntelGpu READ hasIntelGpu CONSTANT)
  Q_PROPERTY(bool nvencAvailable READ nvencAvailable CONSTANT)
  Q_PROPERTY(bool vaapiAvailable READ vaapiAvailable CONSTANT)
  Q_PROPERTY(bool dmaBufAvailable READ dmaBufAvailable CONSTANT)
  Q_PROPERTY(QString recommendedEncoderName READ recommendedEncoderName CONSTANT)
  Q_PROPERTY(QString recommendedEncoderDisplayName READ recommendedEncoderDisplayName CONSTANT)
  Q_PROPERTY(QString recommendedEncoderReason READ recommendedEncoderReason CONSTANT)

public:
  explicit HardwareInfo(QObject *parent = nullptr);

  QStringList gpuDescriptions() const { return m_gpuDescriptions; }
  bool hasNvidiaGpu() const { return m_hasNvidia; }
  bool hasAmdGpu() const { return m_hasAmd; }
  bool hasIntelGpu() const { return m_hasIntel; }
  bool nvencAvailable() const { return m_nvencAvailable; }
  bool vaapiAvailable() const { return m_vaapiAvailable; }
  bool dmaBufAvailable() const { return m_dmaBufAvailable; }
  QString recommendedEncoderName() const { return m_recommendedName; }
  /** Display name ("NVENC H.264") for the recommended encoder id. */
  QString recommendedEncoderDisplayName() const;
  QString recommendedEncoderReason() const { return m_recommendedReason; }

  /** PCI vendor id -> marketing name. Pure, unit-tested. */
  static QString vendorNameForId(quint64 vendorId);

  /**
   * @brief pci.ids chip name -> display name.
   * "AD107 [GeForce RTX 4060]" becomes "GeForce RTX 4060"; names without
   * brackets pass through unchanged. Pure, unit-tested.
   */
  static QString marketingNameForChip(const QString &chipName);

  /**
   * @brief PCI device id -> chip/marketing name via the pci.ids database.
   * Empty when the database or entry is missing (caller falls back to the
   * raw id). dbPathOverride is for tests; empty means standard locations.
   */
  static QString deviceNameForIds(quint64 vendorId, const QString &deviceHex,
                                  const QString &dbPathOverride = QString());

private:
  void gatherGpus();
  void gatherEncoders();

  QStringList m_gpuDescriptions;
  bool m_hasNvidia = false;
  bool m_hasAmd = false;
  bool m_hasIntel = false;
  bool m_nvencAvailable = false;
  bool m_vaapiAvailable = false;
  bool m_dmaBufAvailable = false;
  QString m_recommendedName;
  QString m_recommendedReason;
};

} // namespace TrophyBackend
