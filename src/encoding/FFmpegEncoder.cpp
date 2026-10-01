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
#include <cstdio>
#include <cstring>
#include <type_traits>
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
}

#ifdef HAVE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#endif

namespace TrophyBackend {

#ifdef HAVE_EGL
// Stage B: EGL/CUDA DMA-BUF importer for NVENC.
//
// FFmpeg's CUDA hwcontext cannot map DRM_PRIME frames (av_hwframe_map
// returns ENOSYS), so DMA-BUF is imported manually: EGL image from the
// dma-buf fd -> CUDA/EGL registration -> GPU-side pitch copy into a BGR0
// hwframe from the encoder's own pool. XRGB8888 and BGR0 share the same
// little-endian byte layout (B,G,R,x), so the copy needs no conversion;
// NVENC converts BGR0 to YUV internally. No CPU touch, no new link
// dependencies: libcuda is dlopened (no toolkit headers required).

// Minimal CUDA driver-API mirrors (stable ABI, enums as unsigned int).
using CudaDevicePtr = unsigned long long;
using CudaContext = struct CUctx_st *;
using CudaGraphicsResource = struct CUgraphicsResource_st *;
using CudaArray = struct CUarray_st *;
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
  EGLDisplay display = EGL_NO_DISPLAY;
  ~CudaImportState() {
    if (display != EGL_NO_DISPLAY)
      eglTerminate(display);
    if (cudaLib)
      dlclose(cudaLib);
  }
#endif
  bool probed = false;
  bool available = false;
  QString error;
};

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
  st.display = openEglDisplayForNode("/dev/dri/renderD128");
  if (st.display == EGL_NO_DISPLAY)
    return fail(QStringLiteral("no EGL display"));
  if (eglInitialize(st.display, nullptr, nullptr) != EGL_TRUE)
    return fail(QStringLiteral("eglInitialize failed"));
  const char *exts = eglQueryString(st.display, EGL_EXTENSIONS);
  if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import"))
    return fail(QStringLiteral("EGL dmabuf-import unsupported"));
  st.available = true;
  qDebug() << "FFmpegEncoder: EGL/CUDA DMA-BUF import ready";
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
  cp.dstDevice = reinterpret_cast<CudaDevicePtr>(out->data[0]);
  cp.dstPitch = static_cast<size_t>(out->linesize[0]);
  cp.WidthInBytes = static_cast<size_t>(frame.width) * 4;
  cp.Height = static_cast<size_t>(frame.height);
  mapRet = st.memcpy2D(&cp);
  if (mapRet != 0) {
    QString msg = QStringLiteral("cuMemcpy2D: ") + cudaErrorString(st, mapRet);
    return fail(msg);
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
  m_zeroCopyFrames = 0;
  m_eglImportFrames = 0;
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
  m_codecCtx->framerate = {config.fps, 1};
  m_codecCtx->gop_size = config.fps * 2;

  if (isHardwareEncoder()) {
    m_codecCtx->pix_fmt = m_codecName.contains("nvenc") ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
    if (!initializeHardwareContext()) return false;
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

  return ret >= 0;
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
  if (frame.isDmaBuf && isHardwareEncoder() && m_codecCtx->hw_frames_ctx) {
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
        drm_frame->width = frame.width;
        drm_frame->height = frame.height;
        drm_frame->format = AV_PIX_FMT_DRM_PRIME;
        drm_frame->data[0] = (uint8_t *)desc;
        
        drm_frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc), 
            [](void*, uint8_t* data) {
                auto *d = reinterpret_cast<AVDRMFrameDescriptor*>(data);
                if (d->objects[0].fd >= 0) close(d->objects[0].fd);
                av_free(d);
            }, nullptr, 0);

        tempHwFrame = av_frame_alloc();
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
              qWarning() << "FFmpegEncoder: DMA-BUF hwmap failed for"
                         << m_codecName << "ret:" << mapRet << errStr
                         << "drmFormat:" << QString::number(frame.drmFormat, 16)
                         << "modifier:" << QString::number(frame.drmModifier, 16)
                         << "- using CPU fallback";
              m_lastMapSucceeded = false;
            }
            av_frame_free(&tempHwFrame);
            tempHwFrame = nullptr;
        }
    }
  } 
  
  // 2. Stage-B EGL/CUDA import (NVENC only): DMA-BUF -> BGR0 hwframe with
  // no CPU touch. Attempted when the generic hwmap above failed.
  enum class FrameSource { Cpu, HwMap, EglImport };
  FrameSource source = finalFrame ? FrameSource::HwMap : FrameSource::Cpu;
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
      if (!m_lastImportSucceeded) {
        qDebug() << "FFmpegEncoder: EGL/CUDA import recovered for"
                 << m_codecName;
        m_lastImportSucceeded = true;
      }
    } else if (m_lastImportSucceeded) {
      qWarning() << "FFmpegEncoder: EGL/CUDA import failed for" << m_codecName
                 << m_cudaImport->error << "- using CPU fallback";
      m_lastImportSucceeded = false;
    }
  }
#endif

  // 3. Software Fallback Path
  if (!finalFrame && frame.data) {
    if (av_frame_make_writable(m_frame) >= 0) {
        const uint8_t *srcData[4] = {frame.data, nullptr, nullptr, nullptr};
        int srcStride[4] = {(int)frame.stride, 0, 0, 0};
        
        if (!m_swsCtx) {
            m_swsCtx = sws_getContext(frame.width, frame.height, (AVPixelFormat)frame.pixelFormat,
                                      m_config.width, m_config.height,
                                      (AVPixelFormat)m_frame->format,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr);
        }

        if (m_swsCtx) {
            sws_scale(m_swsCtx, srcData, srcStride, 0, frame.height, m_frame->data, m_frame->linesize);
            
            if (isHardwareEncoder()) {
                tempHwFrame = av_frame_alloc();
                if (av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, tempHwFrame, 0) >= 0) {
                    if (av_hwframe_transfer_data(tempHwFrame, m_frame, 0) >= 0) {
                        finalFrame = tempHwFrame;
                    } else {
                        av_frame_free(&tempHwFrame);
                        tempHwFrame = nullptr;
                    }
                }
            } else {
                finalFrame = m_frame;
            }
        } else {
            qWarning() << "FFmpegEncoder: Failed to create sws context for" << frame.width << "x" << frame.height << "fmt" << frame.pixelFormat;
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
    }
  } else {
    // Neither the DMA-BUF hwmap nor the CPU fallback produced a frame.
    // This is the GPU-only DMA-BUF case (no mapped CPU pointer) with a
    // mapper that cannot import the buffer (e.g. CUDA ENOSYS).
    m_droppedFrames++;
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
      m_zeroCopyFrames + m_eglImportFrames + m_fallbackFrames + m_droppedFrames;
  if (total == 0)
    return;
  qDebug() << "FFmpegEncoder: path summary for" << m_codecName
           << "zero-copy:" << m_zeroCopyFrames
           << "egl-import:" << m_eglImportFrames
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