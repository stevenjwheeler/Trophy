#include "WindowService.h"
#include <QDebug>
#include <QFile>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace GameRecorder {

WindowService::WindowService(QObject *parent) : QObject(parent) {
  m_display = XOpenDisplay(nullptr);
}

void WindowService::setTrophyWindowId(unsigned long windowId) {
  m_trophyWindowId = windowId;
}

WindowService::~WindowService() {
  if (m_display) {
    XCloseDisplay(static_cast<Display *>(m_display));
  }
}

QString WindowService::getWindowTitle(unsigned long windowId) {
  return fetchWindowTitle(windowId);
}

QString WindowService::fetchWindowTitle(unsigned long windowId) {
  if (!m_display)
    return QString();

  Display *dpy = static_cast<Display *>(m_display);
  Window win = static_cast<Window>(windowId);
  QString title;

  // Prefer _NET_WM_NAME (UTF-8) — modern apps like Spotify update this
  // dynamically (e.g. current track) while leaving WM_NAME static.
  Atom netWmName = XInternAtom(dpy, "_NET_WM_NAME", False);
  Atom utf8String = XInternAtom(dpy, "UTF8_STRING", False);
  Atom type;
  int format;
  unsigned long nitems, after;
  unsigned char *data = nullptr;

  if (XGetWindowProperty(dpy, win, netWmName, 0, 1024, False, utf8String, &type,
                         &format, &nitems, &after, &data) == Success) {
    if (data) {
      title = QString::fromUtf8((char *)data);
      XFree(data);
    }
  }

  // Fall back to legacy WM_NAME
  if (title.isEmpty()) {
    char *name = nullptr;
    if (XFetchName(dpy, win, &name)) {
      title = QString::fromLocal8Bit(name);
      XFree(name);
    } else {
      XTextProperty prop;
      if (XGetWMName(dpy, win, &prop)) {
        if (prop.value && prop.nitems > 0) {
          title = QString::fromLocal8Bit((char *)prop.value);
        }
        XFree(prop.value);
      }
    }
  }

  return title;
}

unsigned long WindowService::getPidForWindow(unsigned long windowId) {
  if (!m_display || windowId == 0)
    return 0;

  Display *dpy = static_cast<Display *>(m_display);
  Window win = static_cast<Window>(windowId);

  Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", True);
  if (pidAtom == None)
    return 0;

  Atom type;
  int format;
  unsigned long nitems, after;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy, win, pidAtom, 0, 1, False, XA_CARDINAL, &type,
                         &format, &nitems, &after, &data) != Success)
    return 0;

  if (!data || nitems == 0) {
    if (data)
      XFree(data);
    return 0;
  }

  unsigned long pid = *reinterpret_cast<unsigned long *>(data);
  XFree(data);
  return pid;
}

QString WindowService::getProcessNameForWindow(unsigned long windowId) {
  if (windowId == 0)
    return QString();

  unsigned long pid = getPidForWindow(windowId);
  if (pid == 0)
    return QString();

  QFile commFile(QString("/proc/%1/comm").arg(pid));
  if (!commFile.open(QIODevice::ReadOnly | QIODevice::Text))
    return QString();

  return QString::fromUtf8(commFile.readAll()).trimmed();
}

QString WindowService::getWindowClassForWindow(unsigned long windowId) {
  if (!m_display || windowId == 0)
    return QString();

  Display *dpy = static_cast<Display *>(m_display);
  XClassHint classHint;
  if (!XGetClassHint(dpy, static_cast<Window>(windowId), &classHint))
    return QString();

  QString cls = QString::fromLocal8Bit(classHint.res_class);
  XFree(classHint.res_name);
  XFree(classHint.res_class);
  return cls;
}

QVariantList WindowService::getVisibleWindows() {
  QVariantList windowList;
  if (!m_display)
    return windowList;

  Display *dpy = static_cast<Display *>(m_display);
  Window root = DefaultRootWindow(dpy);
  Window root_ret, parent_ret;
  Window *children = nullptr;
  unsigned int nchildren = 0;

  if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
    return windowList;
  }

  // Temporary storage: PID -> List of candidate windows
  QMap<unsigned long, QList<QVariantMap>> pidGroups;

  for (unsigned int i = 0; i < nchildren; i++) {
    if (children[i] == m_trophyWindowId)
      continue;

    XClassHint classHint;
    if (!XGetClassHint(dpy, children[i], &classHint))
      continue;
    QString windowClass = QString::fromLocal8Bit(classHint.res_class);
    XFree(classHint.res_name);
    XFree(classHint.res_class);

    // 1. Immediate Steam Client Filter (The main app)
    if (windowClass.toLower() == "steam" &&
        fetchWindowTitle(children[i]) == "Steam")
      continue;

    QString title = fetchWindowTitle(children[i]);
    if (title.isEmpty())
      continue;

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, children[i], &attrs) || attrs.width < 320 ||
        attrs.height < 200)
      continue;

    unsigned long pid = getPidForWindow((unsigned long)children[i]);
    if (pid == 0)
      continue;

    QVariantMap info;
    info["id"] = (qlonglong)children[i];
    info["title"] = title;
    info["class"] = windowClass;
    info["pid"] = (qlonglong)pid;

    pidGroups[pid].append(info);
  }

  if (children)
    XFree(children);

  // 2. The Winner Selection Logic
  for (auto it = pidGroups.begin(); it != pidGroups.end(); ++it) {
    QList<QVariantMap> &candidates = it.value();

    if (candidates.size() == 1) {
      // Only one window for this PID? Make sure it's not a generic "Steam"
      // label
      if (candidates[0]["title"].toString() != "Steam") {
        windowList.append(candidates[0]);
      }
    } else {
      // Multiple windows for one PID (The "RACCOIN" vs "Steam" situation)
      QVariantMap bestWindow;
      int bestScore = -1;

      for (const QVariantMap &win : candidates) {
        QString title = win["title"].toString();
        int score = 0;

        // Penalty for generic names
        if (title == "Steam")
          score -= 10;

        // Bonus for length (Games usually have longer names than "Steam")
        score += title.length();

        if (score > bestScore) {
          bestScore = score;
          bestWindow = win;
        }
      }

      if (!bestWindow.isEmpty()) {
        windowList.append(bestWindow);
      }
    }
  }

  // Final debug log of the survivors
  for (const QVariant &v : windowList) {
    QVariantMap m = v.toMap();
    qDebug() << "Detected window:" << m["title"].toString() << "["
             << m["class"].toString() << "]";
  }

  return windowList;
}

} // namespace GameRecorder
