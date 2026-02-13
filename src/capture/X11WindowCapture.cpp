/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDebug>
#include <chrono>

// X11 headers must be included here and macros must be undefined immediately
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#undef Status
#undef Bool
#undef None

#include "X11WindowCapture.h"

// Non-fatal X11 error handler
static int X11ErrorHandler(Display *dpy, XErrorEvent *err) {
  char msg[256];
  XGetErrorText(dpy, err->error_code, msg, sizeof(msg));
  qWarning() << "X11 Error:" << msg
             << "(Request code:" << (int)err->request_code << ")";
  return 0;
}

namespace GameRecorder {

X11WindowCapture::X11WindowCapture(QObject *parent)
    : IVideoCaptureSource(parent) {
  m_shmInfo = new XShmSegmentInfo();
}

X11WindowCapture::~X11WindowCapture() {
  stopCapture();

  // Cleanup X11 and SHM resources
  if (m_display) {
    if (m_shmImage && m_shmInfo) {
      XShmDetach(static_cast<Display *>(m_display),
                 static_cast<XShmSegmentInfo *>(m_shmInfo));
      XDestroyImage(static_cast<XImage *>(m_shmImage));
      shmdt(static_cast<XShmSegmentInfo *>(m_shmInfo)->shmaddr);
      shmctl(static_cast<XShmSegmentInfo *>(m_shmInfo)->shmid, IPC_RMID,
             nullptr);
      m_shmImage = nullptr;
    }
    XCloseDisplay(static_cast<Display *>(m_display));
    m_display = nullptr;
  }
  delete static_cast<XShmSegmentInfo *>(m_shmInfo);
}

bool X11WindowCapture::initialize(const CaptureConfig &config) {
  m_config = config;

  m_display = XOpenDisplay(nullptr);
  if (!m_display) {
    qCritical() << "Failed to open X11 display";
    return false;
  }

  // Install custom error handler to prevent abort on non-fatal errors (like
  // window disappearing)
  XSetErrorHandler(X11ErrorHandler);

  if (!XShmQueryExtension(static_cast<Display *>(m_display))) {
    qCritical() << "X11 Shared Memory extension not supported";
    XCloseDisplay(static_cast<Display *>(m_display));
    m_display = nullptr;
    return false;
  }

  m_targetWindow = m_config.windowId
                       ? (Window)m_config.windowId
                       : DefaultRootWindow(static_cast<Display *>(m_display));
  XWindowAttributes attrs;
  if (XGetWindowAttributes(static_cast<Display *>(m_display), m_targetWindow,
                           &attrs) == 0) {
    qCritical() << "Failed to get window attributes";
    XCloseDisplay(static_cast<Display *>(m_display));
    m_display = nullptr;
    return false;
  }
  m_resolution = QSize(attrs.width, attrs.height);

  // Allocate Shared Memory Image
  m_shmImage = XShmCreateImage(static_cast<Display *>(m_display), attrs.visual,
                               attrs.depth, ZPixmap, nullptr,
                               static_cast<XShmSegmentInfo *>(m_shmInfo),
                               m_resolution.width(), m_resolution.height());
  if (!m_shmImage) {
    qCritical() << "Failed to create XShm image";
    XCloseDisplay(static_cast<Display *>(m_display));
    m_display = nullptr;
    return false;
  }

  XImage *img = static_cast<XImage *>(m_shmImage);
  XShmSegmentInfo *shmInfo = static_cast<XShmSegmentInfo *>(m_shmInfo);

  shmInfo->shmid =
      shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);
  if (shmInfo->shmid == -1) {
    qCritical() << "Failed to allocate shared memory segment";
    XDestroyImage(img);
    m_shmImage = nullptr;
    XCloseDisplay(static_cast<Display *>(m_display));
    m_display = nullptr;
    return false;
  }

  shmInfo->shmaddr = img->data = (char *)shmat(shmInfo->shmid, nullptr, 0);
  shmInfo->readOnly = False;
  if (shmInfo->shmaddr == (char *)-1) {
    qCritical() << "Failed to attach shared memory segment";
    shmctl(shmInfo->shmid, IPC_RMID, nullptr);
    XDestroyImage(img);
    m_shmImage = nullptr;
    XCloseDisplay(static_cast<Display *>(m_display));
    m_display = nullptr;
    return false;
  }

  if (!XShmAttach(static_cast<Display *>(m_display), shmInfo)) {
    qCritical() << "Failed to attach X11 to shared memory";
    shmdt(shmInfo->shmaddr);
    shmctl(shmInfo->shmid, IPC_RMID, nullptr);
    XDestroyImage(img);
    m_shmImage = nullptr;
    XCloseDisplay(static_cast<Display *>(m_display));
    m_display = nullptr;
    return false;
  }

  // Mark for deletion on detach
  shmctl(shmInfo->shmid, IPC_RMID, nullptr);

  qInfo() << "X11 capture initialized with SHM:" << m_resolution.width() << "x"
          << m_resolution.height();
  return true;
}

void X11WindowCapture::setFrameCallback(FrameCallback callback) {
  m_callback = std::move(callback);
}

bool X11WindowCapture::startCapture() {
  if (m_capturing || !m_display || !m_shmImage) {
    return false;
  }

  m_capturing = true;
  m_captureThread = std::thread([this]() { captureLoop(); });

  emit captureStarted();
  qInfo() << "X11 capture started";
  return true;
}

void X11WindowCapture::stopCapture() {
  if (!m_capturing) {
    return;
  }

  m_capturing = false;
  if (m_captureThread.joinable()) {
    m_captureThread.join();
  }

  emit captureStopped();
  qInfo() << "X11 capture stopped";
}

bool X11WindowCapture::isCapturing() const { return m_capturing; }

CaptureCapabilities X11WindowCapture::capabilities() const {
  CaptureCapabilities caps;
  caps.supportsWindowCapture = true;
  caps.supportsMonitorCapture = true;
  caps.supportsRegionCapture = true;
  caps.supportsCursorCapture = true;
  caps.supportsHardwareFrames = false;
  caps.maxFps = 60;
  return caps;
}

QSize X11WindowCapture::nativeResolution() const { return m_resolution; }

void X11WindowCapture::captureLoop() {
  auto frameInterval = std::chrono::microseconds(1000000 / m_config.targetFps);
  Display *disp = static_cast<Display *>(m_display);
  XImage *img = static_cast<XImage *>(m_shmImage);

  int errorCount = 0;
  while (m_capturing) {
    auto frameStart = std::chrono::steady_clock::now();

    bool success = false;
    XWindowAttributes attrs;
    if (XGetWindowAttributes(disp, m_targetWindow, &attrs) &&
        attrs.map_state == IsViewable) {
      if (XShmGetImage(disp, m_targetWindow, img, 0, 0, AllPlanes)) {
        success = true;
        errorCount = 0;
        if (m_callback) {
          VideoFrame frame;
          frame.data = (uint8_t *)img->data;
          frame.width = m_resolution.width();
          frame.height = m_resolution.height();
          frame.stride = img->bytes_per_line;
          frame.timestamp =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();

          // Use BGRA as X11's default format
          frame.pixelFormat = 28; // AV_PIX_FMT_BGRA

          m_callback(frame);
        }
      }
    }

    if (!success) {
      errorCount++;
      if (errorCount % 60 == 0) {
        qWarning() << "X11 capture failing (window minimized or closed?)";
      }
    }

    auto frameEnd = std::chrono::steady_clock::now();
    auto elapsed = frameEnd - frameStart;

    if (elapsed < frameInterval) {
      std::this_thread::sleep_for(frameInterval - elapsed);
    }
  }
}

} // namespace GameRecorder
