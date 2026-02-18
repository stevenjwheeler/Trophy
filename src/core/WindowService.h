#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace GameRecorder {

/**
 * @brief Service to list available windows for manual recording
 */
class WindowService : public QObject {
  Q_OBJECT

public:
  explicit WindowService(QObject *parent = nullptr);
  ~WindowService() override;

  /**
   * @brief Get a list of visible windows
   * @return List of maps containing window info (id, title, class, pid)
   */
  Q_INVOKABLE QVariantList getVisibleWindows();
  Q_INVOKABLE QString getWindowTitle(unsigned long windowId);

  /**
   * @brief Get the process name for a window by its process ID
   */
  QString getProcessNameForWindow(unsigned long windowId);

  /**
   * @brief Get the X11 WM_CLASS resource class for a window (e.g. "Spotify")
   */
  QString getWindowClassForWindow(unsigned long windowId);

  /**
   * @brief Set the Trophy application window to exclude from listings
   */
  void setTrophyWindowId(unsigned long windowId);

private:
  QString fetchWindowTitle(unsigned long windowId);
  unsigned long getPidForWindow(unsigned long windowId);

private:
  void *m_display = nullptr;
  unsigned long m_trophyWindowId = 0;
};

} // namespace GameRecorder
