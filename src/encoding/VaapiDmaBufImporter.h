/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "capture/IVideoCaptureSource.h"
#include <QString>
#include <climits>
#include <dlfcn.h>
#include <limits>
#include <sys/stat.h>
#include <libdrm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

namespace TrophyBackend {

// Explicit PRIME_2 only: FFmpeg's hwmap may retry a rejected modifier through
// legacy DRM_PRIME, which cannot describe compressed auxiliary planes.
// The caller must finish VPP before destroying this source surface.
class VaapiDmaBufImporter {
public:
  struct Ops {
    decltype(&vaCreateSurfaces) create = nullptr;
    decltype(&vaDestroySurfaces) destroy = nullptr;
  };
  VaapiDmaBufImporter() {
    m_library = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    if (m_library) {
      m_ops.create = reinterpret_cast<decltype(m_ops.create)>(dlsym(m_library, "vaCreateSurfaces"));
      m_ops.destroy = reinterpret_cast<decltype(m_ops.destroy)>(dlsym(m_library, "vaDestroySurfaces"));
    }
  }
  explicit VaapiDmaBufImporter(Ops ops) : m_ops(ops) {}
  ~VaapiDmaBufImporter() {
    release();
    if (m_library) dlclose(m_library);
  }
  VaapiDmaBufImporter(const VaapiDmaBufImporter &) = delete;
  VaapiDmaBufImporter &operator=(const VaapiDmaBufImporter &) = delete;

  static bool describe(const VideoFrame &frame, VADRMPRIMESurfaceDescriptor &out,
                       QString &error) {
    out = {};
    auto fail = [&](const char *why) { error = QString::fromLatin1(why); return false; };
    const auto layout = frame.dmaBufLayout();
    if (!frame.isDmaBuf || frame.width <= 0 || frame.height <= 0 ||
        frame.width > 65535 || frame.height > 65535 ||
        frame.drmFormat != DRM_FORMAT_XRGB8888 ||
        !layout.count || layout.count > DmaBufLayout::maxPlanes ||
        frame.drmModifier == DRM_FORMAT_MOD_INVALID)
      return fail("Unsupported explicit DMA-BUF layout");
    const auto &primary = layout.planes[0];
    if (primary.stride < int64_t(frame.width) * 4)
      return fail("Invalid DMA-BUF image pitch");
    struct stat allocation{};
    if (primary.fd < 0 || fstat(primary.fd, &allocation) != 0)
      return fail("Invalid DMA-BUF allocation handle");
    // DMA-BUF exporters may report st_size=0 and SPA maxsize=0. SEEK_END is
    // the kernel DMA-BUF size query; never guess compressed metadata extents.
    const int64_t size = allocation.st_size > 0 ? allocation.st_size
                                               : lseek(primary.fd, 0, SEEK_END);
    if (size <= 0 || uint64_t(size) > std::numeric_limits<uint32_t>::max())
      return fail("Unknown or oversized DMA-BUF allocation");
    for (uint32_t i = 0; i < layout.count; ++i) {
      const auto &plane = layout.planes[i];
      struct stat actual{};
      if (plane.fd < 0 || plane.stride <= 0 || fstat(plane.fd, &actual) != 0)
        return fail("Invalid DMA-BUF plane handle or pitch");
      if (actual.st_dev != allocation.st_dev || actual.st_ino != allocation.st_ino)
        return fail("Multi-object DMA-BUF import is not supported");
      if (uint64_t(plane.offset) >= uint64_t(size) ||
          uint64_t(plane.stride) > uint64_t(size) - plane.offset ||
          plane.size > uint64_t(size))
        return fail("DMA-BUF plane exceeds its allocation");
    }
    // Row geometry is provable for LINEAR. Tiled/compressed plane extents
    // depend on the modifier and are validated by the driver at PRIME_2 import.
    if (frame.drmModifier == DRM_FORMAT_MOD_LINEAR &&
        (layout.count != 1 || uint64_t(primary.stride) * frame.height >
                                  uint64_t(size) - primary.offset))
      return fail("Invalid linear DMA-BUF extent");
    out.fourcc = VA_FOURCC_BGRX;
    out.width = frame.width; out.height = frame.height;
    out.num_objects = 1;
    out.objects[0].fd = primary.fd; // Borrowed until create returns; m_frame owns it.
    out.objects[0].size = uint32_t(size);
    out.objects[0].drm_format_modifier = frame.drmModifier;
    out.num_layers = 1;
    out.layers[0].drm_format = frame.drmFormat;
    out.layers[0].num_planes = layout.count;
    for (uint32_t i = 0; i < layout.count; ++i) {
      out.layers[0].object_index[i] = 0;
      out.layers[0].pitch[i] = layout.planes[i].stride;
      out.layers[0].offset[i] = layout.planes[i].offset;
    }
    error.clear();
    return true;
  }

  bool import(VADisplay display, const VideoFrame &frame) {
    if (!release()) return false; // Bound cleanup failure to one retained surface.
    if (!display || !m_ops.create || !m_ops.destroy) {
      m_error = QStringLiteral("VA-API PRIME_2 import unavailable");
      return false;
    }
    m_frame = frame;
    if (!m_frame.retainDmaBuf()) {
      m_frame = {};
      m_error = QStringLiteral("DMA-BUF ownership failed");
      return false;
    }
    VADRMPRIMESurfaceDescriptor descriptor{};
    if (!describe(m_frame, descriptor, m_error)) {
      m_frame = {};
      return false;
    }
    VASurfaceAttrib attributes[2]{};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &descriptor;
    m_display = display;
    const auto status = m_ops.create(display, VA_RT_FORMAT_RGB32,
        frame.width, frame.height, &m_surface, 1, attributes, 2);
    if (status != VA_STATUS_SUCCESS) {
      // No legacy import retry. On failure libva has not created a surface.
      m_surface = VA_INVALID_SURFACE;
      m_error = QStringLiteral("vaCreateSurfaces(PRIME_2) failed (VA status %1)").arg(status);
      release();
      return false;
    }
    m_error.clear();
    return true;
  }
  VASurfaceID surface() const { return m_surface; }
  const QString &error() const { return m_error; }

  bool release() {
    if (m_surface != VA_INVALID_SURFACE) {
      const auto status = m_ops.destroy(m_display, &m_surface, 1);
      if (status != VA_STATUS_SUCCESS) {
        m_error = QStringLiteral("vaDestroySurfaces(PRIME_2) failed (VA status %1)").arg(status);
        return false; // Keep source handles/lease until a later cleanup retry.
      }
    }
    m_surface = VA_INVALID_SURFACE;
    m_display = nullptr;
    m_frame = {};
    return true;
  }

private:
  Ops m_ops;
  void *m_library = nullptr;
  VADisplay m_display = nullptr;
  VASurfaceID m_surface = VA_INVALID_SURFACE;
  VideoFrame m_frame{};
  QString m_error;
};
} // namespace TrophyBackend
