/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared EGL DMA-BUF helpers (header-only so both the PipeWire capture
 * source and the NVENC importer can use them without new link units).
 * All functionality is compiled only when HAVE_EGL is defined; otherwise
 * the stubs below keep call sites building.
 */

#pragma once

#include <QDebug>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef HAVE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

namespace TrophyBackend {

#ifdef HAVE_EGL
// Open the EGL display for the GPU behind drmNodePath
// (e.g. "/dev/dri/renderD128"). DEFAULT_DISPLAY may resolve to Mesa/llvmpipe,
// which reports only LINEAR and prints dri2-screen warnings. The NVIDIA EGL
// device reports its primary node (e.g. /dev/dri/card1), never the render
// node, hence the scoring (verified 2026-09-30).
inline EGLDisplay openEglDisplayForNode(const char *drmNodePath) {
  auto queryDevices =
      reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(
          eglGetProcAddress("eglQueryDevicesEXT"));
  auto getPlatformDisplay =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));
  auto queryDeviceString =
      reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
          eglGetProcAddress("eglQueryDeviceStringEXT"));
  if (queryDevices && getPlatformDisplay && queryDeviceString) {
    EGLint numDevices = 0;
    if (queryDevices(0, nullptr, &numDevices) && numDevices > 0) {
      std::vector<EGLDeviceEXT> devices(
          static_cast<size_t>(std::min<EGLint>(numDevices, 8)));
      if (queryDevices(static_cast<EGLint>(devices.size()), devices.data(),
                       &numDevices)) {
        int best = -1, bestScore = -1;
        for (int i = 0; i < std::min<int>(numDevices, devices.size()); ++i) {
          const char *node =
              queryDeviceString(devices[i], EGL_DRM_DEVICE_FILE_EXT);
          const char *vendor =
              queryDeviceString(devices[i], EGL_VENDOR);
          qDebug() << "Trophy EGL: device" << i << "drm:"
                   << (node ? node : "(null)")
                   << "vendor:" << (vendor ? vendor : "(null)");
          int score = -1;
          if (node && strcmp(node, drmNodePath) == 0) {
            score = 100;
          } else if (vendor && strstr(vendor, "NVIDIA") && node &&
                     strstr(node, "/dev/dri/") == node) {
            score = 50;
          } else if (node && strstr(node, "renderD") != nullptr) {
            score = 40;
          } else if (node && strstr(node, "/dev/dri/") == node) {
            score = 30;
          } else if (vendor && strstr(vendor, "NVIDIA")) {
            score = 20;
          }
          if (score > bestScore) {
            bestScore = score;
            best = i;
          }
        }
        if (best >= 0 && bestScore >= 0) {
          EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT,
                                              devices[best], nullptr);
          if (dpy != EGL_NO_DISPLAY)
            return dpy;
        }
      }
    }
  }
  return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

// Ask the EGL driver which DMA-BUF modifiers it can import for drmFormat.
// This is a consumer capability, not a guarantee the compositor can allocate
// them. Empty when unavailable (caller offers shared memory only).
inline std::vector<uint64_t> queryDmaBufModifiers(uint32_t drmFormat) {
  std::vector<uint64_t> out;
  EGLDisplay dpy = openEglDisplayForNode("/dev/dri/renderD128");
  if (dpy == EGL_NO_DISPLAY)
    return out;
  if (eglInitialize(dpy, nullptr, nullptr) != EGL_TRUE)
    return out;
  const char *vendor = eglQueryString(dpy, EGL_VENDOR);
  qDebug() << "Trophy EGL: display vendor:" << (vendor ? vendor : "(unknown)");
  const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
  if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
    eglTerminate(dpy);
    return out;
  }
  auto queryFn = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
      eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
  if (!queryFn) {
    eglTerminate(dpy);
    return out;
  }
  EGLint count = 0;
  if (queryFn(dpy, static_cast<EGLint>(drmFormat), 0, nullptr, nullptr,
              &count) == EGL_TRUE &&
      count > 0) {
    count = std::min<EGLint>(count, 64);
    std::vector<EGLuint64KHR> mods(static_cast<size_t>(count));
    EGLint got = 0;
    if (queryFn(dpy, static_cast<EGLint>(drmFormat), count, mods.data(),
                nullptr, &got) == EGL_TRUE &&
        got > 0) {
      out.assign(mods.begin(), mods.begin() + std::min(got, count));
    }
  }
  eglTerminate(dpy);
  return out;
}
#else
inline std::vector<uint64_t> queryDmaBufModifiers(uint32_t) {
  return {};
}
#endif

} // namespace TrophyBackend
