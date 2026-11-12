/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QString>
#include <atomic>
#include <memory>
#include <sys/stat.h>

namespace TrophyBackend {

// Worker-local staged repair. Destruction removes only its own temporary file;
// publication is an owner-thread operation after the library rechecks its gates.
class RecordingRecovery {
public:
  ~RecordingRecovery();
  bool prepare(const QString &path, const std::atomic_bool &cancel);
  bool publish();
  QString error() const { return m_error; }

private:
  QString m_source, m_temporary, m_error;
  struct stat m_original {};
  bool m_ready = false;
};
} // namespace TrophyBackend
