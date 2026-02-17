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

  if (XGetWindowProperty(dpy, win, netWmName, 0, 1024, False, utf8String,
                         &type, &format, &nitems, &after, &data) == Success) {
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

QVariantList WindowService::getVisibleWindows() {
  QVariantList windowList;
  if (!m_display) {
    qWarning() << "Failed to open X11 display for window listing";
    return windowList;
  }

  Display *dpy = static_cast<Display *>(m_display);
  Window root = DefaultRootWindow(dpy);
  Window root_ret, parent_ret;
  Window *children;
  unsigned int nchildren;

  qInfo() << "Scanning X11 windows...";

  if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
    qInfo() << "Found" << nchildren << "top-level X11 windows";
    for (unsigned int i = 0; i < nchildren; i++) {
      // Skip Trophy's own window
      if (children[i] == m_trophyWindowId) {
        continue;
      }

      XWindowAttributes attrs;
      if (XGetWindowAttributes(dpy, children[i], &attrs)) {
        if (attrs.map_state == IsViewable && attrs.width > 32 &&
            attrs.height > 32) {

          QString title = fetchWindowTitle(children[i]);

          // If still no title, might be a shell/launcher, or just unnamed
          if (title.isEmpty()) {
            title = QString("Window %1").arg((qlonglong)children[i]);
          }

          XClassHint classHint;
          bool hasClass = false;
          QString windowClass;
          if (XGetClassHint(dpy, children[i], &classHint)) {
            windowClass = QString::fromLocal8Bit(classHint.res_class);
            XFree(classHint.res_name);
            XFree(classHint.res_class);
            hasClass = !windowClass.isEmpty();
          }

          // Filter out windows that have no title and no class (usually system
          // noise)
          if (title.startsWith("Window ") && !hasClass) {
            continue;
          }

          unsigned long pid = getPidForWindow((unsigned long)children[i]);

          QVariantMap info;
          info["id"] = (qlonglong)children[i];
          info["title"] = title;
          info["class"] = windowClass;
          info["pid"] = (qlonglong)pid;

          qInfo() << "Detected window:" << title << "[" << windowClass << "]";
          windowList.append(info);
        }
      }
    }
    XFree(children);
  }

  return windowList;
}

} // namespace GameRecorder
