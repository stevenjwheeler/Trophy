// Standalone diagnostic baseline, not a Trophy regression test.
// Build/run instructions: qt_media_leak_probe.md.
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMediaPlayer>
#include <QQmlEngine>
#include <QString>

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : "app";
  if (mode != "app" && mode != "engine" && mode != "media")
    return 2;
  {
    if (mode == "engine") {
      QQmlEngine engine;
      engine.collectGarbage();
    } else if (mode == "media") {
      QMediaPlayer player;
      player.stop();
    }
  }
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
  return 0;
}
