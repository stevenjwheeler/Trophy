/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QObject>
#include <QVariantList>
#include <cstdint>
#include <functional>
#include <memory>

namespace TrophyBackend {
// One discovery connection shared by the picker and game status. No capture
// streams, devices, or routing policy are owned here. Public methods run on the
// QObject owner thread; all native discovery runs on its retained worker.
class AudioApplicationRegistry : public QObject {
  Q_OBJECT
public:
  explicit AudioApplicationRegistry(QObject *parent = nullptr);
  ~AudioApplicationRegistry() override;
  void start();
  void stop();
  QVariantList applications() const { return m_applications; }
  bool available() const { return m_available; }
signals:
  void applicationsChanged();
  void availabilityChanged();
private:
  friend class AudioApplicationRegistryTest;
  friend class GameDetectionServiceTest;
  unsigned connectionAttempts() const;
  void publish(quint64 generation, QVariantList applications, bool available);
  struct Private;
  std::unique_ptr<Private> d;
  QVariantList m_applications;
  bool m_available = false, m_running = false;
  quint64 m_generation = 0;
  // Set by the friend fixture before start; immutable until stop joins.
  std::function<void(uint32_t)> m_beforeBindForTesting;
};
} // namespace TrophyBackend
