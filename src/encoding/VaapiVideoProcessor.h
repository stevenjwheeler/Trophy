/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QString>
#include <algorithm>
#include <array>
#include <cstdint>
#include <dlfcn.h>
#include <type_traits>
#include <vector>
#include <va/va.h>
#include <va/va_vpp.h>

namespace TrophyBackend {

// Narrow native boundary: injectable for daemon/GPU-free lifecycle tests.
// The caller owns the display and surfaces and must retain them until process
// returns. VPP is synchronous at this boundary, including error drainage.
class VaapiVideoProcessor {
public:
  struct Ops {
    decltype(&vaCreateConfig) createConfig = nullptr;
    decltype(&vaDestroyConfig) destroyConfig = nullptr;
    decltype(&vaCreateContext) createContext = nullptr;
    decltype(&vaDestroyContext) destroyContext = nullptr;
    decltype(&vaQueryVideoProcPipelineCaps) queryCaps = nullptr;
    decltype(&vaCreateBuffer) createBuffer = nullptr;
    decltype(&vaDestroyBuffer) destroyBuffer = nullptr;
    decltype(&vaBeginPicture) begin = nullptr;
    decltype(&vaRenderPicture) render = nullptr;
    decltype(&vaEndPicture) end = nullptr;
    decltype(&vaSyncSurface) sync = nullptr;
  };

  VaapiVideoProcessor() {
    m_library = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!m_library) return;
#define TROPHY_VA_LOAD(member, symbol) \
    m_ops.member = reinterpret_cast<decltype(m_ops.member)>(dlsym(m_library, symbol))
    TROPHY_VA_LOAD(createConfig, "vaCreateConfig");
    TROPHY_VA_LOAD(destroyConfig, "vaDestroyConfig");
    TROPHY_VA_LOAD(createContext, "vaCreateContext");
    TROPHY_VA_LOAD(destroyContext, "vaDestroyContext");
    TROPHY_VA_LOAD(queryCaps, "vaQueryVideoProcPipelineCaps");
    TROPHY_VA_LOAD(createBuffer, "vaCreateBuffer");
    TROPHY_VA_LOAD(destroyBuffer, "vaDestroyBuffer");
    TROPHY_VA_LOAD(begin, "vaBeginPicture");
    TROPHY_VA_LOAD(render, "vaRenderPicture");
    TROPHY_VA_LOAD(end, "vaEndPicture");
    TROPHY_VA_LOAD(sync, "vaSyncSurface");
#undef TROPHY_VA_LOAD
  }
  explicit VaapiVideoProcessor(Ops ops) : m_ops(ops) {}
  ~VaapiVideoProcessor() {
    reset();
    if (m_library) dlclose(m_library);
  }
  VaapiVideoProcessor(const VaapiVideoProcessor &) = delete;
  VaapiVideoProcessor &operator=(const VaapiVideoProcessor &) = delete;

  const QString &error() const { return m_error; }

  bool initialize(VADisplay display, int width, int height,
                  const std::vector<VASurfaceID> &targets) {
    if (m_context != VA_INVALID_ID && display == m_display &&
        width == m_width && height == m_height && targets == m_targets)
      return true;
    reset();
    if (!display || width <= 0 || height <= 0 || width > 32767 || height > 32767 ||
        targets.empty() || !completeOps())
      return fail("Unavailable VA-API VPP or invalid output");
    m_display = display;
    m_width = width; m_height = height; m_targets = targets;
    VAStatus status = m_ops.createConfig(display, VAProfileNone,
                                         VAEntrypointVideoProc, nullptr, 0, &m_config);
    if (status != VA_STATUS_SUCCESS) return initFail("vaCreateConfig(VPP)", status);
    status = m_ops.createContext(display, m_config, width, height, 0,
                                 m_targets.data(), int(m_targets.size()), &m_context);
    if (status != VA_STATUS_SUCCESS) return initFail("vaCreateContext(VPP)", status);
    std::array<VAProcColorStandardType, 32> inputColors{}, outputColors{};
    std::array<uint32_t, 64> inputFormats{}, outputFormats{};
    m_caps = {};
    m_caps.input_color_standards = inputColors.data();
    m_caps.num_input_color_standards = inputColors.size();
    m_caps.output_color_standards = outputColors.data();
    m_caps.num_output_color_standards = outputColors.size();
    m_caps.input_pixel_format = inputFormats.data();
    m_caps.num_input_pixel_formats = inputFormats.size();
    m_caps.output_pixel_format = outputFormats.data();
    m_caps.num_output_pixel_formats = outputFormats.size();
    status = m_ops.queryCaps(display, m_context, nullptr, 0, &m_caps);
    if (status != VA_STATUS_SUCCESS) return initFail("vaQueryVideoProcPipelineCaps", status);
    // Match swscale's existing SDR conversion rather than silently changing
    // the colour matrix between GPU-input and SHM portions of one recording.
    // Drivers may replace the supplied array pointer (Mesa returns driver-owned
    // storage). Inspect the returned list, not the original temporary buffer.
    const auto *colors = m_caps.output_color_standards;
    const size_t count = std::min<size_t>(m_caps.num_output_color_standards, outputColors.size());
    if (!colors || !count ||
        std::find(colors, colors + count, VAProcColorStandardBT601) == colors + count)
      return initFail("VPP lacks BT.601 output", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    // Capability arrays may be stack-local or driver-owned; retain only scalar limits.
    m_caps.input_color_standards = m_caps.output_color_standards = nullptr;
    m_caps.input_pixel_format = m_caps.output_pixel_format = nullptr;
    m_error.clear();
    return true;
  }

  // Padded draws require a same-canvas, limited-range NV12 black surface.
  // Clear explicitly: some drivers ignore output_background_color outside fit.
  bool process(VASurfaceID source, VASurfaceID output, int sw, int sh,
               VARectangle fit, VASurfaceID black = VA_INVALID_SURFACE) {
    if (!validGeometry(source, output, sw, sh, fit)) return false;
    const bool padded = fit.x || fit.y || fit.width != m_width || fit.height != m_height;
    if (padded) {
      if (black == VA_INVALID_SURFACE || black == source || black == output)
        return fail("Missing or aliased VPP black surface");
      const VARectangle canvas{0, 0, uint16_t(m_width), uint16_t(m_height)};
      if (!validGeometry(black, output, m_width, m_height, canvas) ||
          !submit(black, output, m_width, m_height, canvas, false))
        return false;
    }
    return submit(source, output, sw, sh, fit, true);
  }

private:
  bool validGeometry(VASurfaceID source, VASurfaceID output, int sw, int sh,
                     VARectangle fit) {
    if (m_context == VA_INVALID_ID || source == VA_INVALID_SURFACE ||
        std::find(m_targets.begin(), m_targets.end(), output) == m_targets.end() ||
        sw <= 0 || sh <= 0 || sw > 65535 || sh > 65535 ||
        fit.x < 0 || fit.y < 0 || !fit.width || !fit.height ||
        int(fit.x) + fit.width > m_width || int(fit.y) + fit.height > m_height ||
        ((fit.x | fit.y | fit.width | fit.height) & 1))
      return fail("Invalid VPP source/output geometry");
    auto inRange = [](int value, uint32_t minimum, uint32_t maximum) {
      return (!minimum || uint32_t(value) >= minimum) &&
             (!maximum || uint32_t(value) <= maximum);
    };
    if (!inRange(sw, m_caps.min_input_width, m_caps.max_input_width) ||
        !inRange(sh, m_caps.min_input_height, m_caps.max_input_height) ||
        !inRange(m_width, m_caps.min_output_width, m_caps.max_output_width) ||
        !inRange(m_height, m_caps.min_output_height, m_caps.max_output_height))
      return fail("VPP dimensions outside driver capabilities");
    return true;
  }

  bool submit(VASurfaceID source, VASurfaceID output, int sw, int sh,
              VARectangle fit, bool rgb) {
    // A failed native destroy retains only this one buffer for retry; never
    // accumulate parameter buffers after repeated driver errors.
    if (releaseBuffer() != VA_STATUS_SUCCESS)
      return fail("vaDestroyBuffer(VPP retry)");

    VARectangle input{0, 0, uint16_t(sw), uint16_t(sh)};
    VAProcPipelineParameterBuffer params{};
    params.surface = source;
    params.surface_region = &input;
    params.output_region = &fit;
    params.output_background_color = 0xff000000; // advisory; padded output is explicitly cleared above
    params.surface_color_standard = rgb ? VAProcColorStandardNone : VAProcColorStandardBT601;
    params.output_color_standard = VAProcColorStandardBT601;
    params.input_color_properties.color_range = rgb ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
    params.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
    VAStatus status = m_ops.createBuffer(m_display, m_context,
        VAProcPipelineParameterBufferType, sizeof(params), 1, &params, &m_buffer);
    if (status != VA_STATUS_SUCCESS) return fail("vaCreateBuffer(VPP)", status);
    status = m_ops.begin(m_display, m_context, output);
    if (status == VA_STATUS_SUCCESS) {
      const VAStatus render = m_ops.render(m_display, m_context, &m_buffer, 1);
      // Balance a successful Begin even when Render fails. Settle native
      // consumers before the caller releases its imported DMA-BUF/lease.
      const VAStatus end = m_ops.end(m_display, m_context);
      const VAStatus sync = m_ops.sync(m_display, output);
      status = render != VA_STATUS_SUCCESS ? render : end != VA_STATUS_SUCCESS ? end : sync;
      if (status != VA_STATUS_SUCCESS)
        m_ops.sync(m_display, source);
    }
    const VAStatus destroy = releaseBuffer();
    if (status == VA_STATUS_SUCCESS) status = destroy;
    if (status != VA_STATUS_SUCCESS) return fail("VA-API VPP processing", status);
    m_error.clear();
    return true;
  }

  bool completeOps() const {
    return m_ops.createConfig && m_ops.destroyConfig && m_ops.createContext &&
           m_ops.destroyContext && m_ops.queryCaps && m_ops.createBuffer &&
           m_ops.destroyBuffer && m_ops.begin && m_ops.render && m_ops.end && m_ops.sync;
  }
  bool fail(const char *operation, VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED) {
    m_error = QStringLiteral("%1 (VA status %2)").arg(operation).arg(status);
    return false;
  }
  bool initFail(const char *operation, VAStatus status) {
    fail(operation, status);
    reset();
    return false;
  }
  VAStatus releaseBuffer() {
    if (m_buffer == VA_INVALID_ID) return VA_STATUS_SUCCESS;
    const VAStatus status = m_ops.destroyBuffer(m_display, m_buffer);
    if (status == VA_STATUS_SUCCESS) m_buffer = VA_INVALID_ID;
    return status;
  }
  void reset() {
    releaseBuffer();
    if (m_context != VA_INVALID_ID) m_ops.destroyContext(m_display, m_context);
    if (m_config != VA_INVALID_ID) m_ops.destroyConfig(m_display, m_config);
    m_context = m_config = VA_INVALID_ID;
    m_buffer = VA_INVALID_ID;
    m_display = nullptr;
    m_targets.clear();
  }
  Ops m_ops;
  void *m_library = nullptr;
  VADisplay m_display = nullptr;
  VAConfigID m_config = VA_INVALID_ID;
  VAContextID m_context = VA_INVALID_ID;
  VABufferID m_buffer = VA_INVALID_ID;
  VAProcPipelineCaps m_caps{};
  int m_width = 0, m_height = 0;
  std::vector<VASurfaceID> m_targets;
  QString m_error;
};
} // namespace TrophyBackend
