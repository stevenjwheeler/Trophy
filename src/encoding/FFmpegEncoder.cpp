/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FFmpegEncoder.h"
#include "capture/EglDmaBuf.h"
#include "capture/IVideoCaptureSource.h"

#include <QDebug>
#include <QString>
#include <QRect>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <type_traits>
#include <new>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libdrm/drm_fourcc.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#ifdef HAVE_VAAPI
#include <libavutil/hwcontext_vaapi.h>
#endif
}

#ifdef HAVE_VAAPI
#include "VaapiVideoProcessor.h"
#endif

#ifdef HAVE_EGL
#include "CudaScalePtx.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#endif

namespace TrophyBackend {

namespace {
QRect fitVideoFrame(int sw, int sh, int dw, int dh, int alignW = 1, int alignH = 1) {
  int width = dw;
  int height = dh;
  if (int64_t(sw) * height > int64_t(sh) * width)
    height = std::max(1, int(int64_t(sh) * width / sw));
  else
    width = std::max(1, int(int64_t(sw) * height / sh));
  width = std::max(alignW, width / alignW * alignW);
  height = std::max(alignH, height / alignH * alignH);
  if (width > dw || height > dh)
    return {};
  return QRect(((dw - width) / 2) / alignW * alignW,
               ((dh - height) / 2) / alignH * alignH, width, height);
}
}

#ifdef HAVE_EGL
// Stage B: EGL/CUDA DMA-BUF importer for NVENC.
//
// FFmpeg's CUDA hwcontext cannot map DRM_PRIME frames (av_hwframe_map
// returns ENOSYS), so DMA-BUF is imported manually: EGL image from the
// dma-buf fd -> CUDA/EGL registration -> GPU-side pitch copy into a BGR0
// hwframe from the encoder's own pool (via GPU fit/pad when sizes differ).
// XRGB8888 and BGR0 share the same little-endian byte layout (B,G,R,x);
// NVENC converts BGR0 to YUV internally. No CPU touch, no new link
// dependencies: libcuda is dlopened (no toolkit headers required).

// Minimal CUDA driver-API mirrors (stable ABI, enums as unsigned int).
using CudaDevicePtr = unsigned long long;
using CudaContext = struct CUctx_st *;
using CudaGraphicsResource = struct CUgraphicsResource_st *;
using CudaArray = struct CUarray_st *;
using CudaModule = struct CUmod_st *;
using CudaFunction = struct CUfunc_st *;
using CudaStream = struct CUstream_st *;
struct CudaEglFrame {
  union {
    CudaArray pArray[3];
    CudaDevicePtr pPitch[3];
  } frame;
  unsigned int width, height, depth, pitch, planeCount, numChannels;
  unsigned int frameType; // 1 == PITCH
  unsigned int eglColorFormat;
  unsigned int cuFormat;
};
struct CudaMemcpy2D {
  // NB: coordinates and pitches are size_t (8 bytes on 64-bit); only the
  // memory types are 4-byte enums. Packing these as 32-bit makes the driver
  // read garbage and fail with INVALID_VALUE.
  size_t srcXInBytes, srcY;
  unsigned int srcMemoryType; // 2 == device, 3 == array
  const void *srcHost;
  CudaDevicePtr srcDevice;
  CudaArray srcArray;
  size_t srcPitch;
  size_t dstXInBytes, dstY;
  unsigned int dstMemoryType;
  void *dstHost;
  CudaDevicePtr dstDevice;
  CudaArray dstArray;
  size_t dstPitch;
  size_t WidthInBytes, Height;
};
#endif

struct CudaImportState {
#ifdef HAVE_EGL
  void *cudaLib = nullptr;
  int (*registerImage)(CudaGraphicsResource *, void *, unsigned int) = nullptr;
  int (*unregisterResource)(CudaGraphicsResource) = nullptr;
  int (*getMappedEglFrame)(CudaEglFrame *, CudaGraphicsResource, unsigned int,
                           unsigned int) = nullptr;
  int (*memcpy2D)(const CudaMemcpy2D *) = nullptr;
  int (*ctxPush)(CudaContext) = nullptr;
  int (*ctxPop)(CudaContext *) = nullptr;
  int (*getErrorString)(int, const char **) = nullptr;
  int (*moduleLoad)(CudaModule *, const void *) = nullptr;
  int (*moduleFunction)(CudaFunction *, CudaModule, const char *) = nullptr;
  int (*moduleUnload)(CudaModule) = nullptr;
  int (*allocPitch)(CudaDevicePtr *, size_t *, size_t, size_t, unsigned int) = nullptr;
  int (*freeMemory)(CudaDevicePtr) = nullptr;
  int (*launch)(CudaFunction, unsigned int, unsigned int, unsigned int,
                unsigned int, unsigned int, unsigned int, unsigned int,
                CudaStream, void **, void **) = nullptr;
  int (*synchronize)() = nullptr;
  CudaModule scaleModule = nullptr;
  CudaFunction scaleFunction = nullptr;
  CudaDevicePtr scaleInput = 0;
  size_t scalePitch = 0;
  int scaleWidth = 0, scaleHeight = 0;
  CudaContext scaleContext = nullptr;
  AVBufferRef *scaleDevice = nullptr;
  EglDisplayLease displayLease;
  EGLDisplay display = EGL_NO_DISPLAY;
  ~CudaImportState() {
    if (scaleDevice) {
      if (ctxPush(scaleContext) == 0) {
        synchronize();
        if (scaleInput) freeMemory(scaleInput);
        if (scaleModule) moduleUnload(scaleModule);
        CudaContext previous = nullptr;
        ctxPop(&previous);
      }
      av_buffer_unref(&scaleDevice);
    }
    displayLease.reset();
    if (cudaLib)
      dlclose(cudaLib);
  }
#endif
  bool probed = false;
  bool available = false;
  QString error;
};

struct VaapiImportState {
#ifdef HAVE_VAAPI
  std::unique_ptr<VaapiVideoProcessor> processor;
  AVBufferRef *device = nullptr;
  AVBufferRef *inputFrames = nullptr;
  AVFrame *black = nullptr;
  ~VaapiImportState() {
    // Destroy VPP contexts before the VADisplay/device or pools disappear.
    processor.reset();
    av_frame_free(&black);
    av_buffer_unref(&inputFrames);
    av_buffer_unref(&device);
  }
#endif
  QString error;
};

#ifdef HAVE_VAAPI
namespace {
bool ensureVaapiBlackFrame(VaapiImportState &state, AVBufferRef *outputFrames) {
  if (state.black && state.black->hw_frames_ctx &&
      state.black->hw_frames_ctx->data == outputFrames->data)
    return true;
  av_frame_free(&state.black);
  auto *pool = reinterpret_cast<AVHWFramesContext *>(outputFrames->data);
  AVFrame *software = av_frame_alloc();
  state.black = av_frame_alloc();
  auto fail = [&] {
    av_frame_free(&software);
    av_frame_free(&state.black);
    state.error = QStringLiteral("VA-API black surface allocation/upload failed");
    return false;
  };
  if (!software || !state.black) return fail();
  software->format = AV_PIX_FMT_NV12;
  software->width = pool->width; software->height = pool->height;
  if (av_frame_get_buffer(software, 32) < 0) return fail();
  // Upload once per output pool, never read back captured frames. Each padded
  // input then clears its output with a GPU VPP copy of this retained surface.
  memset(software->data[0], 16, size_t(software->linesize[0]) * software->height);
  memset(software->data[1], 128, size_t(software->linesize[1]) * ((software->height + 1) / 2));
  if (av_hwframe_get_buffer(outputFrames, state.black, 0) < 0 ||
      av_hwframe_transfer_data(state.black, software, 0) < 0)
    return fail();
  av_frame_free(&software);
  return true;
}

AVFrame *vaapiImportScale(VaapiImportState &state, const VideoFrame &input,
                          AVBufferRef *device, AVBufferRef *outputFrames) {
  AVFrame *drm = nullptr, *mapped = nullptr, *output = nullptr;
  auto fail = [&](const QString &error) -> AVFrame * {
    state.error = error;
    av_frame_free(&output);
    av_frame_free(&mapped);
    av_frame_free(&drm);
    return nullptr;
  };
  if (input.drmFormat != DRM_FORMAT_XRGB8888 || input.dmaBufFd < 0 ||
      input.width <= 0 || input.height <= 0 || input.width > 65535 || input.height > 65535 ||
      input.dmaBufStride < int64_t(input.width) * 4 || input.dmaBufOffset < 0)
    return fail(QStringLiteral("Unsupported VA-API DMA-BUF descriptor"));
  if (!state.device) state.device = av_buffer_ref(device);
  if (!state.device) return fail(QStringLiteral("VA-API device reference failed"));
  auto *dev = reinterpret_cast<AVHWDeviceContext *>(device->data);
  auto *native = static_cast<AVVAAPIDeviceContext *>(dev->hwctx);
  auto *pool = reinterpret_cast<AVHWFramesContext *>(outputFrames->data);
  auto *surfaces = static_cast<AVVAAPIFramesContext *>(pool->hwctx);
  if (!native || !native->display || !surfaces || !surfaces->surface_ids ||
      surfaces->nb_surfaces <= 0 || pool->sw_format != AV_PIX_FMT_NV12)
    return fail(QStringLiteral("VA-API VPP requires a fixed output surface pool"));
  if (!state.processor) state.processor = std::make_unique<VaapiVideoProcessor>();
  const std::vector<VASurfaceID> targets(surfaces->surface_ids,
                                        surfaces->surface_ids + surfaces->nb_surfaces);
  if (!state.processor->initialize(native->display, pool->width, pool->height, targets))
    return fail(state.processor->error());

  // Import RGB into a separate source-sized context, NOT the encoder's NV12
  // pool. A successful RGB import alone is not an encodable NV12 surface.
  if (state.inputFrames) {
    auto *frames = reinterpret_cast<AVHWFramesContext *>(state.inputFrames->data);
    if (frames->width != input.width || frames->height != input.height)
      av_buffer_unref(&state.inputFrames);
  }
  if (!state.inputFrames) {
    state.inputFrames = av_hwframe_ctx_alloc(device);
    if (!state.inputFrames) return fail(QStringLiteral("VA-API import pool allocation failed"));
    auto *frames = reinterpret_cast<AVHWFramesContext *>(state.inputFrames->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_BGR0;
    frames->width = input.width;
    frames->height = input.height;
    if (av_hwframe_ctx_init(state.inputFrames) < 0) {
      av_buffer_unref(&state.inputFrames);
      return fail(QStringLiteral("VA-API RGB import context initialization failed"));
    }
  }
  drm = av_frame_alloc(); mapped = av_frame_alloc(); output = av_frame_alloc();
  if (!drm || !mapped || !output) return fail(QStringLiteral("VA-API frame allocation failed"));
  auto *desc = static_cast<AVDRMFrameDescriptor *>(av_mallocz(sizeof(AVDRMFrameDescriptor)));
  if (!desc) return fail(QStringLiteral("VA-API DRM descriptor allocation failed"));
  auto *lease = new (std::nothrow) std::shared_ptr<void>(input.bufferLease);
  const int fd = dup(input.dmaBufFd);
  if (!lease || fd < 0) {
    delete lease; if (fd >= 0) close(fd); av_free(desc);
    return fail(QStringLiteral("VA-API DMA-BUF ownership failed"));
  }
  desc->nb_objects = 1;
  desc->objects[0].fd = fd;
  desc->objects[0].size = size_t(input.dmaBufStride) * input.height + input.dmaBufOffset;
  desc->objects[0].format_modifier = input.drmModifier;
  desc->nb_layers = 1;
  desc->layers[0].format = input.drmFormat;
  desc->layers[0].nb_planes = 1;
  desc->layers[0].planes[0].pitch = input.dmaBufStride;
  desc->layers[0].planes[0].offset = input.dmaBufOffset;
  drm->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(desc), sizeof(*desc),
      [](void *opaque, uint8_t *bytes) {
        auto *descriptor = reinterpret_cast<AVDRMFrameDescriptor *>(bytes);
        close(descriptor->objects[0].fd);
        av_free(descriptor);
        delete static_cast<std::shared_ptr<void> *>(opaque);
      }, lease, 0);
  if (!drm->buf[0]) {
    delete lease; close(fd); av_free(desc);
    return fail(QStringLiteral("VA-API DRM buffer allocation failed"));
  }
  drm->data[0] = reinterpret_cast<uint8_t *>(desc);
  drm->format = AV_PIX_FMT_DRM_PRIME;
  drm->width = input.width; drm->height = input.height;
  mapped->format = AV_PIX_FMT_VAAPI;
  mapped->hw_frames_ctx = av_buffer_ref(state.inputFrames);
  if (!mapped->hw_frames_ctx || av_hwframe_map(mapped, drm, AV_HWFRAME_MAP_READ) < 0)
    return fail(QStringLiteral("VA-API DMA-BUF RGB import failed"));
  if (av_hwframe_get_buffer(outputFrames, output, 0) < 0)
    return fail(QStringLiteral("VA-API output surface allocation failed"));
  const QRect fit = fitVideoFrame(input.width, input.height, pool->width, pool->height, 2, 2);
  const VARectangle region{int16_t(fit.x()), int16_t(fit.y()),
                            uint16_t(fit.width()), uint16_t(fit.height())};
  const bool padded = fit != QRect(0, 0, pool->width, pool->height);
  if (padded && !ensureVaapiBlackFrame(state, outputFrames))
    return fail(state.error);
  if (!state.processor->process(VASurfaceID(uintptr_t(mapped->data[3])),
                                VASurfaceID(uintptr_t(output->data[3])),
                                input.width, input.height, region,
                                padded ? VASurfaceID(uintptr_t(state.black->data[3])) : VA_INVALID_SURFACE))
    return fail(state.processor->error());
  // process() settles VPP before these references release the producer lease.
  av_frame_free(&mapped);
  av_frame_free(&drm);
  return output;
}
}
#endif

#ifdef HAVE_EGL
namespace {

QString cudaErrorString(CudaImportState &st, int code) {
  const char *msg = nullptr;
  if (st.getErrorString && st.getErrorString(code, &msg) == 0 && msg)
    return QString::fromUtf8(msg);
  return QStringLiteral("CUDA error %1").arg(code);
}

bool cudaImportEnsure(CudaImportState &st) {
  if (st.probed)
    return st.available;
  st.probed = true;
  auto fail = [&st](const QString &msg) {
    st.error = msg;
    st.available = false;
    st.displayLease.reset();
    st.display = EGL_NO_DISPLAY;
    return false;
  };
  st.cudaLib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!st.cudaLib)
    return fail(QStringLiteral("dlopen libcuda.so.1 failed"));
  // Versioned entry points first (_v2), then unversioned.
  auto resolve = [&st](auto &fn, const char *v2, const char *v1,
                       const char *what) -> bool {
    fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(st.cudaLib, v2));
    if (!fn && v1)
      fn = reinterpret_cast<std::decay_t<decltype(fn)>>(
          dlsym(st.cudaLib, v1));
    if (!fn) {
      st.error = QStringLiteral("libcuda missing %1").arg(what);
      st.available = false;
      return false;
    }
    return true;
  };
  if (!resolve(st.registerImage, "cuGraphicsEGLRegisterImage",
               "cuGraphicsEGLRegisterImage", "cuGraphicsEGLRegisterImage"))
    return false;
  if (!resolve(st.unregisterResource, "cuGraphicsUnregisterResource",
               "cuGraphicsUnregisterResource", "cuGraphicsUnregisterResource"))
    return false;
  if (!resolve(st.getMappedEglFrame, "cuGraphicsResourceGetMappedEglFrame",
               "cuGraphicsResourceGetMappedEglFrame",
               "cuGraphicsResourceGetMappedEglFrame"))
    return false;
  if (!resolve(st.memcpy2D, "cuMemcpy2D_v2", "cuMemcpy2D", "cuMemcpy2D"))
    return false;
  if (!resolve(st.ctxPush, "cuCtxPushCurrent_v2", "cuCtxPushCurrent",
               "cuCtxPushCurrent"))
    return false;
  if (!resolve(st.ctxPop, "cuCtxPopCurrent_v2", "cuCtxPopCurrent",
               "cuCtxPopCurrent"))
    return false;
  st.getErrorString =
      reinterpret_cast<decltype(st.getErrorString)>(
          dlsym(st.cudaLib, "cuGetErrorString")); // optional
  st.displayLease = acquireEglDisplay(openEglDisplayForNode("/dev/dri/renderD128"));
  if (!st.displayLease)
    return fail(QStringLiteral("EGL display acquisition/initialization failed"));
  st.display = st.displayLease.get();
  const char *exts = eglQueryString(st.display, EGL_EXTENSIONS);
  if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import"))
    return fail(QStringLiteral("EGL dmabuf-import unsupported"));
  st.available = true;
  qDebug() << "FFmpegEncoder: EGL/CUDA DMA-BUF import ready";
  return true;
}

// Called only with the encoder's CUDA context current. Resources are bounded
// to one module and one source-sized scratch allocation; no producer buffers
// or registrations are cached. Same-size imports never need this machinery.
bool cudaScaleEnsure(CudaImportState &st, AVBufferRef *device, CudaContext context,
                     int width, int height) {
  auto resolve = [&](auto &fn, const char *name) {
    if (!fn)
      fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(st.cudaLib, name));
    return fn != nullptr;
  };
  if (!resolve(st.moduleLoad, "cuModuleLoadData") ||
      !resolve(st.moduleFunction, "cuModuleGetFunction") ||
      !resolve(st.moduleUnload, "cuModuleUnload") ||
      !resolve(st.allocPitch, "cuMemAllocPitch_v2") ||
      !resolve(st.freeMemory, "cuMemFree_v2") ||
      !resolve(st.launch, "cuLaunchKernel") ||
      !resolve(st.synchronize, "cuCtxSynchronize")) {
    st.error = QStringLiteral("CUDA scaling entry points unavailable");
    return false;
  }
  if (!st.scaleDevice) {
    st.scaleDevice = av_buffer_ref(device);
    if (!st.scaleDevice) {
      st.error = QStringLiteral("CUDA scaling device reference allocation failed");
      return false;
    }
    st.scaleContext = context;
  }
  int ret = 0;
  if (!st.scaleModule)
    ret = st.moduleLoad(&st.scaleModule, cudaScalePtx);
  if (ret == 0 && !st.scaleFunction)
    ret = st.moduleFunction(&st.scaleFunction, st.scaleModule, "trophyScaleBgr0");
  if (ret != 0) {
    st.error = QStringLiteral("CUDA scaling kernel: ") + cudaErrorString(st, ret);
    return false;
  }
  if (st.scaleWidth != width || st.scaleHeight != height) {
    // Allocate before retiring the old scratch; retain at most these two
    // allocations briefly on a size transition, never an unbounded size LRU.
    CudaDevicePtr input = 0;
    size_t pitch = 0;
    ret = st.allocPitch(&input, &pitch, size_t(width) * 4, size_t(height), 16);
    if (ret != 0) {
      st.error = QStringLiteral("CUDA scaling buffer: ") + cudaErrorString(st, ret);
      return false;
    }
    if (st.scaleInput) st.freeMemory(st.scaleInput);
    st.scaleInput = input;
    st.scalePitch = pitch;
    st.scaleWidth = width;
    st.scaleHeight = height;
  }
  return true;
}

// Import a single-plane XRGB8888 DMA-BUF into a BGR0 CUDA hwframe from
// cudaFramesCtx. Returns a new AVFrame owned by the caller, or nullptr
// with st.error set.
AVFrame *cudaImportBgr0(CudaImportState &st, const VideoFrame &frame,
                        AVBufferRef *cudaDeviceCtx,
                        AVBufferRef *cudaFramesCtx) {
  AVFrame *out = nullptr;
  CudaGraphicsResource res = nullptr;
  EGLImage img = EGL_NO_IMAGE;
  int fd = -1;
  bool pushed = false;
  // CUcontext is the first member of AVCUDADeviceContext (hwcontext_cuda.h);
  // read by offset to avoid the <cuda.h> include.
  auto fail = [&](const QString &msg) -> AVFrame * {
    st.error = msg;
    if (res)
      st.unregisterResource(res);
    if (pushed) {
      CudaContext dummy = nullptr;
      st.ctxPop(&dummy);
    }
    if (img != EGL_NO_IMAGE)
      eglDestroyImage(st.display, img);
    if (fd >= 0)
      close(fd);
    if (out)
      av_frame_free(&out);
    return nullptr;
  };

  if (!cudaImportEnsure(st))
    return nullptr;
  if (frame.drmFormat != DRM_FORMAT_XRGB8888 || frame.width <= 0 ||
      frame.height <= 0 || frame.dmaBufStride <= 0 || frame.dmaBufFd < 0) {
    st.error = QStringLiteral("unsupported DMA-BUF descriptor");
    return nullptr;
  }
  auto *devCtx =
      reinterpret_cast<AVHWDeviceContext *>(cudaDeviceCtx->data);
  if (!devCtx || !devCtx->hwctx) {
    st.error = QStringLiteral("no CUDA device context");
    return nullptr;
  }
  CudaContext cuCtx = *reinterpret_cast<CudaContext *>(devCtx->hwctx);
  if (!cuCtx) {
    st.error = QStringLiteral("null CUDA context");
    return nullptr;
  }
  fd = dup(frame.dmaBufFd);
  if (fd < 0) {
    st.error = QStringLiteral("dup(dmabuf) failed");
    return nullptr;
  }
  const EGLAttrib attrs[] = {
      EGL_WIDTH,
      static_cast<EGLAttrib>(frame.width),
      EGL_HEIGHT,
      static_cast<EGLAttrib>(frame.height),
      // NOTE: declared as ARGB, not XRGB. The two share the same
      // little-endian byte layout (B,G,R,x), but CUDA's EGL interop
      // rejects the XRGB fourcc while accepting ARGB (verified 2026-09-30,
      // driver 13040). Only bytes are copied downstream, so the relabel
      // is content-neutral.
      EGL_LINUX_DRM_FOURCC_EXT,
      static_cast<EGLAttrib>(DRM_FORMAT_ARGB8888),
      EGL_DMA_BUF_PLANE0_FD_EXT,
      static_cast<EGLAttrib>(fd),
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      static_cast<EGLAttrib>(frame.dmaBufOffset),
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      static_cast<EGLAttrib>(frame.dmaBufStride),
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
      static_cast<EGLAttrib>(frame.drmModifier & 0xffffffffu),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      static_cast<EGLAttrib>(frame.drmModifier >> 32),
      EGL_NONE,
  };
  img = eglCreateImage(st.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                       nullptr, attrs);
  if (img == EGL_NO_IMAGE)
    return fail(QStringLiteral("eglCreateImage failed"));
  if (st.ctxPush(cuCtx) != 0)
    return fail(QStringLiteral("cuCtxPushCurrent failed"));
  pushed = true;
  int regRet = st.registerImage(&res, img, 0);
  if (regRet != 0) {
    QString msg =
        QStringLiteral("register: ") + cudaErrorString(st, regRet);
    return fail(msg);
  }
  CudaEglFrame ef{};
  int mapRet = st.getMappedEglFrame(&ef, res, 0, 0);
  if (mapRet != 0) {
    QString msg =
        QStringLiteral("getMappedEglFrame: ") + cudaErrorString(st, mapRet);
    return fail(msg);
  }
  if (ef.planeCount < 1)
    return fail(QStringLiteral("EGL frame has no planes"));
  out = av_frame_alloc();
  if (!out)
    return fail(QStringLiteral("av_frame_alloc failed"));
  if (av_hwframe_get_buffer(cudaFramesCtx, out, 0) < 0)
    return fail(QStringLiteral("av_hwframe_get_buffer failed"));
  const bool scale = out->width != frame.width || out->height != frame.height;
  if (scale && !cudaScaleEnsure(st, cudaDeviceCtx, cuCtx, frame.width, frame.height))
    return fail(st.error);
  CudaMemcpy2D cp{};
  if (ef.frameType == 1) {
    // Pitched device memory.
    cp.srcMemoryType = 2;
    cp.srcDevice = ef.frame.pPitch[0];
    cp.srcPitch = ef.pitch;
  } else if (ef.frameType == 0 && ef.frame.pArray[0]) {
    // CUDA array (what the driver returns for ARGB dmabuf images).
    cp.srcMemoryType = 3;
    cp.srcArray = ef.frame.pArray[0];
  } else {
    char detail[96];
    snprintf(detail, sizeof(detail),
             "unsupported EGL frame type %u", ef.frameType);
    return fail(QString::fromUtf8(detail));
  }
  cp.dstMemoryType = 2;
  cp.dstDevice = scale ? st.scaleInput : reinterpret_cast<CudaDevicePtr>(out->data[0]);
  cp.dstPitch = scale ? st.scalePitch : static_cast<size_t>(out->linesize[0]);
  if (cp.dstPitch < size_t(frame.width) * 4)
    return fail(QStringLiteral("CUDA copy destination pitch too small"));
  cp.WidthInBytes = static_cast<size_t>(frame.width) * 4;
  cp.Height = static_cast<size_t>(frame.height);
  mapRet = st.memcpy2D(&cp);
  if (mapRet != 0) {
    QString msg = QStringLiteral("cuMemcpy2D: ") + cudaErrorString(st, mapRet);
    return fail(msg);
  }
  if (scale) {
    const QRect fit = fitVideoFrame(frame.width, frame.height, out->width, out->height);
    CudaDevicePtr dst = reinterpret_cast<CudaDevicePtr>(out->data[0]);
    size_t dstPitch = size_t(out->linesize[0]);
    int sw = frame.width, sh = frame.height, dw = out->width, dh = out->height;
    int x = fit.x(), y = fit.y(), w = fit.width(), h = fit.height();
    void *args[] = {&st.scaleInput, &st.scalePitch, &sw, &sh, &dst, &dstPitch,
                    &dw, &dh, &x, &y, &w, &h};
    mapRet = st.launch(st.scaleFunction, (dw + 15) / 16, (dh + 15) / 16, 1,
                       16, 16, 1, 0, nullptr, args, nullptr);
    // Complete the copy/kernel before scratch reuse, unregistering the EGL
    // source or returning its producer lease. No asynchronous raw pointers
    // may outlive these owned resources.
    const int syncRet = st.synchronize();
    if (mapRet != 0 || syncRet != 0)
      return fail(QStringLiteral("CUDA scaling: ") +
                  cudaErrorString(st, mapRet != 0 ? mapRet : syncRet));
  }
  st.unregisterResource(res);
  res = nullptr;
  CudaContext dummy = nullptr;
  st.ctxPop(&dummy);
  pushed = false;
  eglDestroyImage(st.display, img);
  img = EGL_NO_IMAGE;
  close(fd);
  fd = -1;
  return out;
}

} // namespace
#else
// Without EGL there is no import path; the encoder keeps its CPU fallback.
#endif

FFmpegEncoder::FFmpegEncoder(EncoderType type, const QString &codecName,
                             QObject *parent)
    : IEncoder(parent), m_type(type), m_codecName(codecName) {
  switch (type) {
  case EncoderType::NVENC_H264: m_name = "NVENC H.264"; break;
  case EncoderType::NVENC_HEVC: m_name = "NVENC HEVC"; break;
  case EncoderType::NVENC_AV1:  m_name = "NVENC AV1"; break;
  case EncoderType::VAAPI_H264: m_name = "VA-API H.264"; break;
  case EncoderType::VAAPI_HEVC: m_name = "VA-API HEVC"; break;
  case EncoderType::VAAPI_AV1:  m_name = "VA-API AV1"; break;
  case EncoderType::Software_x264: m_name = "x264"; break;
  }
}

FFmpegEncoder::~FFmpegEncoder() { cleanup(); }

bool FFmpegEncoder::initialize(const EncoderConfig &config) {
  if (m_initialized) cleanup();

  m_config = config;
  
  if (!initializeCodec(config)) {
      qWarning() << "FFmpegEncoder: Failed to initialize codec.";
      return false;
  }

  m_frame = av_frame_alloc();
  m_packet = av_packet_alloc();
  
  if (!m_frame || !m_packet) { 
      cleanup(); 
      return false; 
  }

  // Setup software staging frame. It MUST match the downstream consumer:
  // hardware encoders upload into hwframes, and av_hwframe_transfer_data
  // with a format conversion (e.g. YUV420P -> NV12) silently transfers only
  // half the chroma on this FFmpeg/CUDA stack (verified 2026-09-30).
  // NVENC takes BGR0 natively (converting to YUV in hardware), which also
  // matches the Stage-B EGL import pool, so no CPU subsampling is ever
  // needed there; VAAPI keeps NV12 (verified chroma-correct) and x264
  // keeps YUV420P.
  AVPixelFormat stagingFmt = AV_PIX_FMT_YUV420P;
  if (isHardwareEncoder()) {
    stagingFmt = m_codecName.contains("nvenc") ? AV_PIX_FMT_BGR0
                                               : AV_PIX_FMT_NV12;
  }
  m_frame->format = stagingFmt;
  m_frame->width = config.width;
  m_frame->height = config.height;

  if (av_frame_get_buffer(m_frame, 0) < 0) {
      qWarning() << "FFmpegEncoder: Failed to allocate software frame buffer.";
      cleanup();
      return false;
  }

  m_frameCount = 0;
  m_firstTimestamp = -1; 
  m_initialized = true;
  // A transient first-probe failure must not permanently disable the
  // EGL/CUDA import for the process: re-probe on every initialize().
  m_cudaImport.reset();
  m_vaapiImport.reset();
  m_zeroCopyFrames = 0;
  m_eglImportFrames = 0;
  m_gpuScaledFrames = 0;
  m_vaapiVppFrames = 0;
  m_fallbackFrames = 0;
  m_droppedFrames = 0;
  m_lastMapSucceeded = true;
  m_lastImportSucceeded = true;
  m_summaryLogged = false;

  return true;
}

bool FFmpegEncoder::initializeCodec(const EncoderConfig &config) {
  const AVCodec *codec = avcodec_find_encoder_by_name(m_codecName.toUtf8().constData());
  if (!codec) return false;

  m_codecCtx = avcodec_alloc_context3(codec);
  if (!m_codecCtx) return false;

  m_codecCtx->width = config.width;
  m_codecCtx->height = config.height;
  m_codecCtx->time_base = {1, 1000};
  // Uncapped input retains capture timestamps. Codecs still need a positive
  // nominal rate for rate control and frame-count-based GOP configuration.
  const int nominalFps = config.fps > 0 ? config.fps : 60;
  m_codecCtx->framerate = {nominalFps, 1};
  // Honor the per-pipeline keyframe interval (DVR segments use 1s for
  // tight clip cuts; recordings keep the 2s default). Was hardcoded.
  m_codecCtx->gop_size = nominalFps * std::max(1, config.keyframeInterval);

  if (isHardwareEncoder()) {
    m_codecCtx->pix_fmt = m_codecName.contains("nvenc") ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
    if (!initializeHardwareContext()) { cleanup(); return false; }
  } else {
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  }

  m_codecCtx->bit_rate = static_cast<int64_t>(config.bitrate) * 1000;
  
  AVDictionary *opts = nullptr;
  if (m_codecName.contains("nvenc")) {
    av_dict_set(&opts, "preset", "p4", 0);
    av_dict_set(&opts, "tune", "ull", 0); 
    av_dict_set(&opts, "delay", "0", 0);
    av_dict_set(&opts, "rc", config.useVBR ? "vbr" : "cbr", 0);
  }

  m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int ret = avcodec_open2(m_codecCtx, codec, &opts);
  av_dict_free(&opts);

  if (ret < 0) { cleanup(); return false; }
  return true;
}

bool FFmpegEncoder::initializeHardwareContext() {
    enum AVHWDeviceType type = m_codecName.contains("nvenc") ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_VAAPI;

    if (av_hwdevice_ctx_create(&m_hwDeviceCtx, type, nullptr, nullptr, 0) < 0) return false;
    
    // Attach device context
    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);

    m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
    if (!m_hwFramesCtx) return false;

    AVHWFramesContext *f = (AVHWFramesContext *)m_hwFramesCtx->data;
    f->format = m_codecCtx->pix_fmt;
    // NVENC accepts BGR0 surfaces (converting to YUV in hardware); this also
    // matches the Stage-B EGL import pool and the BGR0 staging frame, so the
    // CPU fallback never subsamples. VAAPI keeps NV12.
    f->sw_format = m_codecName.contains("nvenc") ? AV_PIX_FMT_BGR0
                                                 : AV_PIX_FMT_NV12;
    f->width = m_codecCtx->width;
    f->height = m_codecCtx->height;
    f->initial_pool_size = 20;

    if (av_hwframe_ctx_init(m_hwFramesCtx) < 0) return false;

    m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
    return true;
}

bool FFmpegEncoder::encodeFrame(const VideoFrame &frame, EncodedPacket &output) {
  if (!m_initialized || !m_codecCtx) return false;

  if (frame.width <= 0 || frame.height <= 0) {
    qWarning() << "FFmpegEncoder: Ignoring invalid frame size" << frame.width << "x" << frame.height;
    return false;
  }

  bool useVaapiVpp = false;
#ifdef HAVE_VAAPI
  useVaapiVpp = m_codecName.contains("vaapi");
#endif
  // Without VPP support, mismatched direct mappings still need SHM.
  if (frame.isDmaBuf &&
      !m_codecName.contains("nvenc") && !useVaapiVpp &&
      (frame.width != m_config.width || frame.height != m_config.height)) {
    if (m_swsCtx) {
      sws_freeContext(m_swsCtx);
      m_swsCtx = nullptr;
    }
    if (frame.width != m_lastInputWidth || frame.height != m_lastInputHeight) {
      qWarning() << "FFmpegEncoder: DMA-BUF size" << frame.width << "x" << frame.height
                 << "differs from output" << m_config.width << "x" << m_config.height
                 << "- requesting SHM for aspect-preserving scaling";
      m_lastInputWidth = frame.width;
      m_lastInputHeight = frame.height;
    }
    ++m_droppedFrames;
    if (frame.requestCpuFallback)
      frame.requestCpuFallback();
    return false;
  }

  if (m_firstTimestamp < 0) m_firstTimestamp = frame.timestamp;
  int64_t relativeMs = (frame.timestamp - m_firstTimestamp) / 1000;

  AVFrame *finalFrame = nullptr; 
  AVFrame *tempHwFrame = nullptr; 
  AVFrame *drm_frame = nullptr;

  // Reset the sws context when input format or resolution changes.
  if (frame.width != m_lastInputWidth || frame.height != m_lastInputHeight ||
      frame.pixelFormat != m_swsInputFmt) {
    if (m_swsCtx) {
      sws_freeContext(m_swsCtx);
      m_swsCtx = nullptr;
    }
    m_lastInputWidth = frame.width;
    m_lastInputHeight = frame.height;
    m_swsInputFmt = frame.pixelFormat;
  }

  // 1. Attempt DMA-BUF Mapping (Zero-copy path)
  if (frame.isDmaBuf && isHardwareEncoder() && m_codecCtx->hw_frames_ctx &&
      !useVaapiVpp &&
      frame.width == m_config.width && frame.height == m_config.height) {
    auto *desc = (AVDRMFrameDescriptor *)av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (desc) {
        // Duplicate FD so we own the lifetime
        desc->nb_objects = 1;
        desc->objects[0].fd = dup(frame.dmaBufFd); 
        desc->objects[0].size = frame.height * frame.dmaBufStride; 
        desc->objects[0].format_modifier = frame.drmModifier;
        
        desc->nb_layers = 1;
        desc->layers[0].format = frame.drmFormat;
        desc->layers[0].nb_planes = 1;
        desc->layers[0].planes[0].object_index = 0;
        desc->layers[0].planes[0].pitch = frame.dmaBufStride;
        desc->layers[0].planes[0].offset = frame.dmaBufOffset;

        drm_frame = av_frame_alloc();
        if (!drm_frame || desc->objects[0].fd < 0) {
          if (desc->objects[0].fd >= 0) close(desc->objects[0].fd);
          av_free(desc);
          av_frame_free(&drm_frame);
          ++m_droppedFrames;
          if (frame.requestCpuFallback) frame.requestCpuFallback();
          return false;
        }
        drm_frame->width = frame.width;
        drm_frame->height = frame.height;
        drm_frame->format = AV_PIX_FMT_DRM_PRIME;
        drm_frame->data[0] = (uint8_t *)desc;
        
        // VA-API may retain the mapped source beyond send_frame. Attach the
        // producer lease to its AVBuffer lifetime, not just this stack frame.
        auto *lease = new std::shared_ptr<void>(frame.bufferLease);
        drm_frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc),
            [](void *opaque, uint8_t* data) {
                auto *d = reinterpret_cast<AVDRMFrameDescriptor*>(data);
                if (d->objects[0].fd >= 0) close(d->objects[0].fd);
                av_free(d);
                delete static_cast<std::shared_ptr<void> *>(opaque);
            }, lease, 0);

        if (!drm_frame->buf[0]) {
            delete lease;
            // Buffer allocation failed: release the descriptor (including
            // the dup'd fd the free callback would have owned) and fall
            // through to the CPU path below.
            if (desc->objects[0].fd >= 0)
                close(desc->objects[0].fd);
            av_free(desc);
            av_frame_free(&drm_frame);
            drm_frame = nullptr;
        }

        if (drm_frame) {
        tempHwFrame = av_frame_alloc();
        if (!tempHwFrame) {
          av_frame_free(&drm_frame);
          ++m_droppedFrames;
          return false;
        }
        tempHwFrame->format = m_codecCtx->pix_fmt;
        tempHwFrame->hw_frames_ctx = av_buffer_ref(m_codecCtx->hw_frames_ctx);

        int mapRet = av_hwframe_map(tempHwFrame, drm_frame, AV_HWFRAME_MAP_READ);
        if (mapRet == 0) {
            finalFrame = tempHwFrame;
            m_zeroCopyFrames++;
            if (!m_lastMapSucceeded) {
              qDebug() << "FFmpegEncoder: DMA-BUF hwmap recovered for"
                       << m_codecName;
              m_lastMapSucceeded = true;
            }
        } else {
            // Log once per failure streak: success->failure transition and
            // the very first failure. Per-frame logging here would spam at
            // 60fps when the mapper is unimplemented (e.g. CUDA ENOSYS).
            if (m_lastMapSucceeded) {
              char errStr[AV_ERROR_MAX_STRING_SIZE] = {0};
              av_strerror(mapRet, errStr, sizeof(errStr));
#ifdef HAVE_EGL
              const bool tryEgl = m_codecName.contains("nvenc");
#else
              const bool tryEgl = false;
#endif
              // An unavailable generic mapper is not a CPU fallback: CUDA
              // normally returns ENOSYS here and EGL/CUDA still imports on GPU.
              qDebug() << "FFmpegEncoder: generic DMA-BUF mapping unavailable for"
                         << m_codecName << "ret:" << mapRet << errStr
                         << "drmFormat:" << QString::number(frame.drmFormat, 16)
                         << "modifier:" << QString::number(frame.drmModifier, 16)
                          << (tryEgl ? "- trying EGL/CUDA import"
                                     : "- checking CPU/SHM fallback");
              m_lastMapSucceeded = false;
            }
            av_frame_free(&tempHwFrame);
            tempHwFrame = nullptr;
        }
        } // if (drm_frame): null buf falls through to the CPU path below
    }
  } 
  
  // 2. Stage-B EGL/CUDA import (NVENC only): DMA-BUF -> BGR0 hwframe with
  // no CPU touch. Attempted when the generic hwmap above failed.
  enum class FrameSource { Cpu, HwMap, EglImport, VaapiVpp };
  FrameSource source = finalFrame ? FrameSource::HwMap : FrameSource::Cpu;
#ifdef HAVE_VAAPI
  if (!finalFrame && useVaapiVpp && frame.isDmaBuf && m_hwDeviceCtx &&
      m_codecCtx->hw_frames_ctx) {
    if (!m_vaapiImport) m_vaapiImport = std::make_unique<VaapiImportState>();
    tempHwFrame = vaapiImportScale(*m_vaapiImport, frame, m_hwDeviceCtx,
                                   m_codecCtx->hw_frames_ctx);
    if (tempHwFrame) {
      finalFrame = tempHwFrame;
      source = FrameSource::VaapiVpp;
      ++m_vaapiVppFrames;
      if (frame.width != m_config.width || frame.height != m_config.height)
        ++m_gpuScaledFrames;
      m_lastImportSucceeded = true;
    } else if (m_lastImportSucceeded) {
      qWarning() << "FFmpegEncoder: VA-API import/VPP failed:" << m_vaapiImport->error
                 << "- requesting SHM fallback";
      m_lastImportSucceeded = false;
    }
  }
#endif
#ifdef HAVE_EGL
  if (!finalFrame && m_codecName.contains("nvenc") && frame.isDmaBuf &&
      m_hwDeviceCtx && m_codecCtx->hw_frames_ctx) {
    if (!m_cudaImport)
      m_cudaImport = std::make_unique<CudaImportState>();
    AVFrame *imported = cudaImportBgr0(*m_cudaImport, frame, m_hwDeviceCtx,
                                       m_codecCtx->hw_frames_ctx);
    if (imported) {
      tempHwFrame = imported;
      finalFrame = tempHwFrame;
      source = FrameSource::EglImport;
      m_eglImportFrames++;
      if (frame.width != m_config.width || frame.height != m_config.height)
        ++m_gpuScaledFrames;
      if (!m_lastImportSucceeded) {
        qDebug() << "FFmpegEncoder: EGL/CUDA import recovered for"
                 << m_codecName;
        m_lastImportSucceeded = true;
      }
    } else if (m_lastImportSucceeded) {
      qWarning() << "FFmpegEncoder: EGL/CUDA import failed for" << m_codecName
                  << m_cudaImport->error << "- checking CPU/SHM fallback";
      m_lastImportSucceeded = false;
    }
  }
#endif

  // 3. Software Fallback Path
  if (!finalFrame && frame.data) {
    if (convertSoftwareFrame(frame)) {
      if (isHardwareEncoder()) {
        tempHwFrame = av_frame_alloc();
        if (tempHwFrame &&
            av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, tempHwFrame, 0) >= 0 &&
            av_hwframe_transfer_data(tempHwFrame, m_frame, 0) >= 0) {
          finalFrame = tempHwFrame;
        } else {
          av_frame_free(&tempHwFrame);
        }
      } else {
        finalFrame = m_frame;
      }
    }
  }

  bool success = false;
  if (finalFrame) {
    if (source == FrameSource::Cpu)
      m_fallbackFrames++;
    finalFrame->pts = av_rescale_q(relativeMs, {1, 1000}, m_codecCtx->time_base);
    int ret = avcodec_send_frame(m_codecCtx, finalFrame);

    if (ret >= 0) {
      m_frameCount++;
      ret = avcodec_receive_packet(m_codecCtx, m_packet);
      if (ret == 0) {
        success = processPacket(m_packet, output);
      } else if (ret == AVERROR(EAGAIN)) {
        // Frame accepted but encoder needs more input before emitting.
        // Report false: no packet was produced, so there is nothing for
        // the caller to mux (writing `output` here would mux garbage).
        success = false;
      }
    } else if (frame.isDmaBuf && frame.requestCpuFallback) {
      // Import success alone does not prove the encoder accepts the mapped
      // surface (e.g. RGB imported into an NV12-only VA-API encoder).
      frame.requestCpuFallback();
    }
  } else {
    // Neither the DMA-BUF hwmap nor the CPU fallback produced a frame.
    // This is the GPU-only DMA-BUF case (no mapped CPU pointer) with a
    // mapper that cannot import the buffer (e.g. CUDA ENOSYS).
    m_droppedFrames++;
    if (frame.isDmaBuf && frame.requestCpuFallback)
      frame.requestCpuFallback();
    if (m_droppedFrames == 1 || m_droppedFrames % 600 == 0) {
      qWarning() << "FFmpegEncoder: dropping frame, no usable path for"
                 << m_codecName << "isDmaBuf:" << frame.isDmaBuf
                 << "hasCpuData:" << (frame.data != nullptr)
                 << "(total dropped:" << m_droppedFrames << ")";
    }
  }

  if (tempHwFrame) av_frame_free(&tempHwFrame);
  if (drm_frame) av_frame_free(&drm_frame);

  return success;
}

bool FFmpegEncoder::convertSoftwareFrame(const VideoFrame &frame) {
  const auto srcFmt = static_cast<AVPixelFormat>(frame.pixelFormat);
  const auto dstFmt = static_cast<AVPixelFormat>(m_frame->format);
  // Reject invalid/unsupported formats before swscale (some builds assert).
  if (!av_pix_fmt_desc_get(srcFmt) || !sws_isSupportedInput(srcFmt) ||
      av_frame_make_writable(m_frame) < 0)
    return false;

  const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(dstFmt);
  if (!desc)
    return false;
  const int alignW = 1 << desc->log2_chroma_w;
  const int alignH = 1 << desc->log2_chroma_h;
  // Chroma planes need aligned extents and origins. Round down, never
  // beyond the output, and retain at least one chroma sample for tiny input.
  const QRect fit = fitVideoFrame(frame.width, frame.height,
                                  m_config.width, m_config.height, alignW, alignH);
  if (fit.isEmpty())
    return false;

  if (!m_swsCtx) {
    // Packed RGB bicubic scaling is exceptionally costly in swscale's
    // NVENC staging path (~23ms/frame for resized 1080p output on our host).
    // Bilinear matches the CUDA resizer's filter and leaves real-time
    // headroom. Keep the existing bicubic YUV paths for x264 and VA-API.
    const int scaleFlags = m_codecName.contains("nvenc") ? SWS_BILINEAR : SWS_BICUBIC;
    m_swsCtx = sws_getContext(frame.width, frame.height, srcFmt,
                             fit.width(), fit.height(), dstFmt,
                             scaleFlags, nullptr, nullptr, nullptr);
  }
  if (!m_swsCtx)
    return false;

  ptrdiff_t strides[4];
  for (int i = 0; i < 4; ++i)
    strides[i] = m_frame->linesize[i];
  // Clear every letterboxed frame, not just on resize: staging storage can
  // hold pixels from a previous size. Full-canvas scaling overwrites it all.
  // This also sets neutral chroma for YUV bars.
  if ((fit.width() != m_config.width || fit.height() != m_config.height) &&
      av_image_fill_black(m_frame->data, strides, dstFmt, AVCOL_RANGE_MPEG,
                          m_config.width, m_config.height) < 0)
    return false;
  uint8_t *dst[4] = {m_frame->data[0], m_frame->data[1], m_frame->data[2], nullptr};
  dst[0] += fit.y() * m_frame->linesize[0] + fit.x() * (dstFmt == AV_PIX_FMT_BGR0 ? 4 : 1);
  if (dstFmt == AV_PIX_FMT_YUV420P) {
    dst[1] += (fit.y() / 2) * m_frame->linesize[1] + fit.x() / 2;
    dst[2] += (fit.y() / 2) * m_frame->linesize[2] + fit.x() / 2;
  } else if (dstFmt == AV_PIX_FMT_NV12) {
    dst[1] += (fit.y() / 2) * m_frame->linesize[1] + fit.x();
  }
  const uint8_t *src[4] = {frame.data, nullptr, nullptr, nullptr};
  const int srcStrides[4] = {int(frame.stride), 0, 0, 0};
  return sws_scale(m_swsCtx, src, srcStrides, 0, frame.height,
                   dst, m_frame->linesize) == fit.height();
}

int FFmpegEncoder::flush(std::vector<EncodedPacket> &packets) {
  if (!m_initialized) return 0;
  logSummary();
  avcodec_send_frame(m_codecCtx, nullptr);
  int count = 0;
  while (avcodec_receive_packet(m_codecCtx, m_packet) >= 0) {
    EncodedPacket out;
    if (processPacket(m_packet, out)) {
      packets.push_back(std::move(out));
      count++;
    }
  }
  return count;
}

void FFmpegEncoder::logSummary() {
  if (m_summaryLogged)
    return;
  m_summaryLogged = true;
  const int64_t total =
      m_zeroCopyFrames + m_eglImportFrames + m_vaapiVppFrames + m_fallbackFrames + m_droppedFrames;
  if (total == 0)
    return;
  qDebug() << "FFmpegEncoder: path summary for" << m_codecName
           << "zero-copy:" << m_zeroCopyFrames
            << "egl-import:" << m_eglImportFrames
            << "GPU-scaled:" << m_gpuScaledFrames
            << "VA-API-VPP:" << m_vaapiVppFrames
           << "CPU-fallback:" << m_fallbackFrames
           << "dropped:" << m_droppedFrames;
}

bool FFmpegEncoder::processPacket(AVPacket *packet, EncodedPacket &output) {
  output.data.resize(packet->size);
  memcpy(output.data.data(), packet->data, packet->size);
  output.pts = av_rescale_q(packet->pts, m_codecCtx->time_base, {1, 1000});
  output.dts = (packet->dts == AV_NOPTS_VALUE) ? output.pts : av_rescale_q(packet->dts, m_codecCtx->time_base, {1, 1000});
  output.duration = av_rescale_q(packet->duration, m_codecCtx->time_base, {1, 1000});
  output.isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
  output.streamIndex = 0;
  av_packet_unref(packet);
  return true;
}

bool FFmpegEncoder::isHardwareEncoder() const {
  return m_type != EncoderType::Software_x264;
}

int FFmpegEncoder::encodedPixelFormat() const {
  if (!m_codecCtx)
    return AV_PIX_FMT_NONE;

  // Hardware encoder contexts use a hardware surface format for input frames,
  // but the encoded H.264/HEVC/AV1 stream is typically represented as YUV420.
  if (isHardwareEncoder())
    return AV_PIX_FMT_YUV420P;

  return m_codecCtx->pix_fmt;
}

std::vector<uint8_t> FFmpegEncoder::extraData() const {
  if (!m_codecCtx || !m_codecCtx->extradata) return {};
  return std::vector<uint8_t>(m_codecCtx->extradata, m_codecCtx->extradata + m_codecCtx->extradata_size);
}

int FFmpegEncoder::codecId() const { return m_codecCtx ? m_codecCtx->codec_id : 0; }

void FFmpegEncoder::cleanup() {
  // Scaler modules/scratch must be retired on their current CUDA context
  // before the encoder's hardware device references are released.
  m_cudaImport.reset();
  m_vaapiImport.reset();
  if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
  m_lastInputWidth = 0;
  m_lastInputHeight = 0;
  m_swsInputFmt = 0;
  if (m_packet) av_packet_free(&m_packet);
  if (m_frame) av_frame_free(&m_frame);
  if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);
  if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
  if (m_codecCtx) avcodec_free_context(&m_codecCtx);
  m_initialized = false;
}

} // namespace TrophyBackend
