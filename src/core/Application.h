/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <memory>

// Forward declarations
namespace GameRecorder {
class RecordingManager;
class SettingsManager;
class StorageManager;
class RecordingLibrary;
class GameDetectionService;
class NotificationService;
class AudioMixer;
class WindowService;
} // namespace GameRecorder

namespace GameRecorder {

/**
 * @brief Main application class that owns all core services
 *
 * Application is responsible for:
 * - Creating and managing the QML engine
 * - Instantiating and owning all service objects
 * - Exposing services to QML via context properties
 * - Handling application lifecycle
 */
class Application : public QGuiApplication {
  Q_OBJECT

  // Properties exposed to QML
  Q_PROPERTY(bool isRecording READ isRecording NOTIFY recordingStateChanged)
  Q_PROPERTY(
      QString currentGameName READ currentGameName NOTIFY currentGameChanged)

public:
  explicit Application(int &argc, char **argv);
  ~Application() override;

  /**
   * @brief Initialize all services and load QML
   * @return true on success, false on failure
   */
  bool initialize();

  // Service accessors
  RecordingManager *recordingManager() const {
    return m_recordingManager.get();
  }
  SettingsManager *settingsManager() const { return m_settingsManager.get(); }
  StorageManager *storageManager() const { return m_storageManager.get(); }
  RecordingLibrary *recordingLibrary() const {
    return m_recordingLibrary.get();
  }
  GameDetectionService *gameDetector() const { return m_gameDetector.get(); }
  NotificationService *notificationService() const {
    return m_notificationService.get();
  }

  // Property getters
  bool isRecording() const;
  QString currentGameName() const;

signals:
  void recordingStateChanged();
  void currentGameChanged();
  void fatalError(const QString &message);

private:
  void initializeServices();
  void registerQmlTypes();
  void exposeServicesToQml();
  void connectSignals();
  void setupSystemTray();

  std::unique_ptr<RecordingManager> m_recordingManager;
  std::unique_ptr<SettingsManager> m_settingsManager;
  std::unique_ptr<StorageManager> m_storageManager;
  std::unique_ptr<RecordingLibrary> m_recordingLibrary;
  std::unique_ptr<GameDetectionService> m_gameDetector;
  std::unique_ptr<NotificationService> m_notificationService;
  std::unique_ptr<AudioMixer> m_audioMixer;
  std::unique_ptr<WindowService> m_windowService;
  std::unique_ptr<QQmlApplicationEngine> m_engine;
};

} // namespace GameRecorder
