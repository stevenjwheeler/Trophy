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
   * @return List of maps containing window info (id, title, class)
   */
  Q_INVOKABLE QVariantList getVisibleWindows();
  Q_INVOKABLE QString getWindowTitle(unsigned long windowId);

private:
  QString fetchWindowTitle(unsigned long windowId);

private:
  void *m_display = nullptr;
};

} // namespace GameRecorder
