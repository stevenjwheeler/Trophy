/*
 * Trophy - Unit Tests
 * Stage B: feed a real GBM-exported XRGB DMA-BUF through FFmpegEncoder /
 * NVENC and assert the EGL/CUDA import path (not hwmap, not CPU fallback)
 * takes it. Skips gracefully without NVIDIA EGL / GBM / NVENC.
 */

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QLoggingCategory>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <gtest/gtest.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libdrm/drm_fourcc.h>
}

#include <cmath>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <barrier>
#include <thread>

#include "capture/IVideoCaptureSource.h"
#include "capture/EglDmaBuf.h"
#include "encoding/FFmpegEncoder.h"
#include "muxing/FFmpegMuxer.h"

using namespace TrophyBackend;

class DmaBufImportTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new DmaBufImportTestEnvironment);

namespace {
class ScopedEncoderMessages {
public:
  ScopedEncoderMessages() {
    auto *category = QLoggingCategory::defaultCategory();
    m_debug = category->isDebugEnabled(); m_warning = category->isWarningEnabled();
    {
      std::lock_guard lock(s_mutex);
      s_messages = &m_messages;
    }
    category->setEnabled(QtDebugMsg, true);
    category->setEnabled(QtWarningMsg, true);
    m_previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &text) {
      std::lock_guard lock(s_mutex);
      if (s_messages) s_messages->emplace_back(type, text);
    });
  }
  ~ScopedEncoderMessages() {
    qInstallMessageHandler(m_previous);
    auto *category = QLoggingCategory::defaultCategory();
    category->setEnabled(QtDebugMsg, m_debug);
    category->setEnabled(QtWarningMsg, m_warning);
    std::lock_guard lock(s_mutex);
    s_messages = nullptr;
  }
  bool contains(QtMsgType type, const QString &text) const {
    std::lock_guard lock(s_mutex);
    return std::any_of(m_messages.begin(), m_messages.end(), [&](const auto &message) {
      return message.first == type && message.second.contains(text);
    });
  }
private:
  inline static std::mutex s_mutex;
  inline static std::vector<std::pair<QtMsgType, QString>> *s_messages = nullptr;
  std::vector<std::pair<QtMsgType, QString>> m_messages;
  QtMessageHandler m_previous = nullptr;
  bool m_debug = false, m_warning = false;
};
}

// EGL device with dma-buf import support present?
static bool eglImportUsable() {
  auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(
      eglGetProcAddress("eglQueryDevicesEXT"));
  auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  auto queryDeviceString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
      eglGetProcAddress("eglQueryDeviceStringEXT"));
  if (!queryDevices || !getPlatformDisplay || !queryDeviceString)
    return false;
  EGLint numDevices = 0;
  if (!queryDevices(0, nullptr, &numDevices) || numDevices <= 0)
    return false;
  EGLDeviceEXT devices[8];
  if (!queryDevices(8, devices, &numDevices))
    return false;
  for (int i = 0; i < numDevices; ++i) {
    EGLDisplay dpy =
        getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
    if (dpy == EGL_NO_DISPLAY)
      continue;
    bool ok = false;
    auto lease = acquireEglDisplay(dpy);
    if (lease) {
      const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
      ok = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
    }
    if (ok)
      return true;
  }
  return false;
}

struct TestDmaBuf {
  int drmFd = -1;
  gbm_device *gbm = nullptr;
  gbm_bo *bo = nullptr;
  int fd = -1;
  int stride = 0;
  uint64_t modifier = 0;
  bool mappedFill = false;
  ~TestDmaBuf() {
    if (fd >= 0)
      close(fd);
    if (bo)
      gbm_bo_destroy(bo);
    if (gbm)
      gbm_device_destroy(gbm);
    if (drmFd >= 0)
      close(drmFd);
  }
};

// NVIDIA tiled BOs need not support gbm_bo_map. Fill them through an EGL
// image/FBO instead, so decoded-content checks really exercise that transport.
static bool fillGpuGradient(TestDmaBuf &buf, int width, int height, bool quadrants) {
  auto lease = acquireEglDisplay(openEglDisplayForNode("/dev/dri/renderD128"));
  if (!lease)
    return false;
  const EGLDisplay display = lease.get();
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  EGLImage image = EGL_NO_IMAGE;
  unsigned texture = 0, framebuffer = 0;
  auto genTextures = reinterpret_cast<void (*)(int, unsigned *)>(eglGetProcAddress("glGenTextures"));
  auto bindTexture = reinterpret_cast<void (*)(unsigned, unsigned)>(eglGetProcAddress("glBindTexture"));
  auto targetImage = reinterpret_cast<void (*)(unsigned, void *)>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  auto genFramebuffers = reinterpret_cast<void (*)(int, unsigned *)>(eglGetProcAddress("glGenFramebuffers"));
  auto bindFramebuffer = reinterpret_cast<void (*)(unsigned, unsigned)>(eglGetProcAddress("glBindFramebuffer"));
  auto framebufferTexture = reinterpret_cast<void (*)(unsigned, unsigned, unsigned, unsigned, int)>(eglGetProcAddress("glFramebufferTexture2D"));
  auto checkFramebuffer = reinterpret_cast<unsigned (*)(unsigned)>(eglGetProcAddress("glCheckFramebufferStatus"));
  auto enable = reinterpret_cast<void (*)(unsigned)>(eglGetProcAddress("glEnable"));
  auto scissor = reinterpret_cast<void (*)(int, int, int, int)>(eglGetProcAddress("glScissor"));
  auto clearColor = reinterpret_cast<void (*)(float, float, float, float)>(eglGetProcAddress("glClearColor"));
  auto clear = reinterpret_cast<void (*)(unsigned)>(eglGetProcAddress("glClear"));
  auto finish = reinterpret_cast<void (*)()>(eglGetProcAddress("glFinish"));
  auto getError = reinterpret_cast<unsigned (*)()>(eglGetProcAddress("glGetError"));
  auto deleteTextures = reinterpret_cast<void (*)(int, const unsigned *)>(eglGetProcAddress("glDeleteTextures"));
  auto deleteFramebuffers = reinterpret_cast<void (*)(int, const unsigned *)>(eglGetProcAddress("glDeleteFramebuffers"));
  auto done = [&](bool result) {
    if (framebuffer) deleteFramebuffers(1, &framebuffer);
    if (texture) deleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE) eglDestroyImage(display, image);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    return result;
  };
  if (!genTextures || !bindTexture || !targetImage || !genFramebuffers ||
      !bindFramebuffer || !framebufferTexture || !checkFramebuffer || !enable ||
      !scissor || !clearColor || !clear || !finish || !getError ||
      !deleteTextures || !deleteFramebuffers)
    return done(false);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return done(false);
  const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
  EGLConfig config;
  EGLint count = 0;
  if (!eglChooseConfig(display, configAttrs, &config, 1, &count) || count != 1)
    return done(false);
  const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
  const EGLint surfaceAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
  if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
      !eglMakeCurrent(display, surface, surface, context))
    return done(false);
  const EGLAttrib attrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height,
      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
      EGL_DMA_BUF_PLANE0_FD_EXT, buf.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, buf.stride,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLAttrib(buf.modifier & 0xffffffffu),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLAttrib(buf.modifier >> 32), EGL_NONE};
  image = eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
  if (image == EGL_NO_IMAGE) return done(false);
  constexpr unsigned texture2d = 0x0DE1, fbo = 0x8D40;
  genTextures(1, &texture);
  bindTexture(texture2d, texture);
  targetImage(texture2d, image);
  genFramebuffers(1, &framebuffer);
  bindFramebuffer(fbo, framebuffer);
  framebufferTexture(fbo, 0x8CE0, texture2d, texture, 0);
  if (checkFramebuffer(fbo) != 0x8CD5) return done(false);
  enable(0x0C11); // GL_SCISSOR_TEST
  if (quadrants) {
    for (int row = 0; row < 2; ++row)
      for (int col = 0; col < 2; ++col) {
        const int x = col * (width / 2), y = row * (height / 2);
        scissor(x, y, col ? width - x : width / 2, row ? height - y : height / 2);
        clearColor((row == col) ? 1 : 0, col ? 1 : 0, row ? 1 : 0, 1);
        clear(0x00004000);
      }
  } else {
    for (int x = 0; x < width; ++x) {
      const float v = float(x) / (width - 1);
      scissor(x, 0, 1, height);
      clearColor(v, v, v, 1);
      clear(0x00004000); // GL_COLOR_BUFFER_BIT
    }
  }
  finish();
  return done(getError() == 0);
}

// Horizontal gradient so decoded luma has a known mean (~mid-grey).
static std::unique_ptr<TestDmaBuf> makeXrgbDmaBuf(int w, int h, bool quadrants = false) {
  auto buf = std::make_unique<TestDmaBuf>();
  buf->drmFd = open("/dev/dri/renderD128", O_RDWR);
  if (buf->drmFd < 0)
    return nullptr;
  buf->gbm = gbm_create_device(buf->drmFd);
  if (!buf->gbm)
    return nullptr;
  buf->bo = gbm_bo_create(buf->gbm, w, h, GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!buf->bo)
    return nullptr;
  buf->fd = gbm_bo_get_fd(buf->bo);
  buf->stride = gbm_bo_get_stride(buf->bo);
  buf->modifier = gbm_bo_get_modifier(buf->bo);
  if (buf->fd < 0 || buf->stride < w * 4)
    return nullptr;
  uint32_t stride = 0;
  void *mapData = nullptr;
  void *ptr = gbm_bo_map(buf->bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride,
                         &mapData);
  if (ptr) {
    for (int y = 0; y < h; ++y) {
      uint8_t *row = static_cast<uint8_t *>(ptr) + y * stride;
      for (int x = 0; x < w; ++x) {
        const uint8_t v = static_cast<uint8_t>((x * 255) / (w - 1));
        const bool right = x >= w / 2, bottom = y >= h / 2;
        row[x * 4 + 0] = quadrants ? (bottom ? 255 : 0) : v;
        row[x * 4 + 1] = quadrants ? (right ? 255 : 0) : v;
        row[x * 4 + 2] = quadrants ? (right == bottom ? 255 : 0) : v;
        row[x * 4 + 3] = 0xFF;
      }
    }
    gbm_bo_unmap(buf->bo, mapData);
    buf->mappedFill = true;
  }
  if (!buf->mappedFill)
    buf->mappedFill = fillGpuGradient(*buf, w, h, quadrants);
  return buf;
}

TEST(DmaBufCudaImportTest, NvencEglImportEncodesDmaBuf) {
  if (avcodec_find_encoder_by_name("h264_nvenc") == nullptr)
    GTEST_SKIP() << "h264_nvenc unavailable";
  if (!eglImportUsable())
    GTEST_SKIP() << "no EGL device with dma-buf import";
  const int W = 640, H = 360;
  auto dma = makeXrgbDmaBuf(W, H);
  if (!dma || dma->fd < 0)
    GTEST_SKIP() << "no GBM XRGB DMA-BUF (no render node?)";

  FFmpegEncoder venc(IEncoder::EncoderType::NVENC_H264, "h264_nvenc");
  EncoderConfig vcfg;
  vcfg.width = W;
  vcfg.height = H;
  vcfg.fps = 30;
  vcfg.bitrate = 2000;
  if (!venc.initialize(vcfg))
    GTEST_SKIP() << "NVENC init failed (no GPU?)";

  QTemporaryDir tempDir;
  ASSERT_TRUE(tempDir.isValid());
  const QString outPath = tempDir.path() + "/nvenc-dmabuf.mkv";
  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(outPath));
  StreamInfo vinfo;
  vinfo.type = StreamInfo::Type::Video;
  vinfo.codecId = venc.codecId();
  vinfo.extraData = venc.extraData();
  vinfo.pixelFormat = venc.encodedPixelFormat();
  vinfo.width = W;
  vinfo.height = H;
  vinfo.fps = 30;
  vinfo.bitrate = 2000;
  const int vs = muxer.addStream(vinfo);
  ASSERT_GE(vs, 0);
  ASSERT_TRUE(muxer.writeHeader());

  ScopedEncoderMessages messages;
  int produced = 0;
  for (int f = 0; f < 30; ++f) {
    VideoFrame frame{};
    frame.width = W;
    frame.height = H;
    frame.timestamp = (int64_t)f * 33333;
    frame.pixelFormat = AV_PIX_FMT_DRM_PRIME;
    frame.isDmaBuf = true;
    frame.dmaBufFd = dma->fd; // borrowed; encoder dups internally
    frame.drmFormat = DRM_FORMAT_XRGB8888;
    frame.drmModifier = dma->modifier;
    frame.dmaBufStride = dma->stride;
    frame.dmaBufOffset = 0;
    EncodedPacket pkt;
    if (venc.encodeFrame(frame, pkt)) {
      pkt.streamIndex = vs;
      EXPECT_TRUE(muxer.writePacket(pkt));
      ++produced;
    }
  }
  std::vector<EncodedPacket> flushed;
  produced += venc.flush(flushed);
  for (auto &pkt : flushed) {
    pkt.streamIndex = vs;
    EXPECT_TRUE(muxer.writePacket(pkt));
  }
  EXPECT_GT(produced, 0);
  // The generic hwmap cannot import into CUDA (ENOSYS); the frames must
  // have gone through the Stage-B EGL import, not the CPU fallback (which
  // has no CPU pointer to work with here).
  EXPECT_EQ(venc.zeroCopyFrames(), 0);
  EXPECT_GT(venc.eglImportFrames(), 0) << "EGL/CUDA import took no frames";
  EXPECT_EQ(venc.fallbackFrames(), 0);
  if (venc.zeroCopyFrames() == 0 && venc.eglImportFrames() > 0) {
    EXPECT_TRUE(messages.contains(QtDebugMsg, "trying EGL/CUDA import"));
    EXPECT_FALSE(messages.contains(QtWarningMsg, "generic DMA-BUF mapping"));
    EXPECT_FALSE(messages.contains(QtWarningMsg, "using CPU fallback"));
    EXPECT_FALSE(messages.contains(QtWarningMsg, "checking CPU/SHM fallback"));
  }
  EXPECT_TRUE(muxer.finalize());

  if (!dma->mappedFill)
    return; // content check needs the mapped gradient fill

  // Decode back and require mid-grey luma: proves imported bytes are real.
  AVFormatContext *ctx = nullptr;
  ASSERT_EQ(
      avformat_open_input(&ctx, outPath.toUtf8().constData(), nullptr, nullptr),
      0);
  ASSERT_EQ(avformat_find_stream_info(ctx, nullptr), 0);
  int vstream = -1;
  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
      vstream = (int)i;
  }
  ASSERT_GE(vstream, 0);
  const AVCodec *dec =
      avcodec_find_decoder(ctx->streams[vstream]->codecpar->codec_id);
  ASSERT_NE(dec, nullptr);
  AVCodecContext *dctx = avcodec_alloc_context3(dec);
  ASSERT_EQ(avcodec_parameters_to_context(dctx, ctx->streams[vstream]->codecpar),
            0);
  ASSERT_EQ(avcodec_open2(dctx, dec, nullptr), 0);
  AVPacket *ap = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  bool gotFrame = false;
  double mean = 0;
  while (!gotFrame && av_read_frame(ctx, ap) >= 0) {
    if (ap->stream_index != vstream) {
      av_packet_unref(ap);
      continue;
    }
    if (avcodec_send_packet(dctx, ap) < 0) {
      av_packet_unref(ap);
      continue;
    }
    av_packet_unref(ap);
    while (avcodec_receive_frame(dctx, fr) == 0) {
      double sum = 0;
      long n = 0;
      for (int y = 0; y < fr->height; y += 8)
        for (int x = 0; x < fr->width; x += 8) {
          sum += fr->data[0][y * fr->linesize[0] + x];
          ++n;
        }
      mean = sum / n;
      gotFrame = true;
      break;
    }
  }
  av_frame_free(&fr);
  av_packet_free(&ap);
  avcodec_free_context(&dctx);
  avformat_close_input(&ctx);
  ASSERT_TRUE(gotFrame) << "no decodable frame in EGL-import output";
  EXPECT_GT(mean, 60.0) << "luma too dark: import content wrong";
  EXPECT_LT(mean, 200.0) << "luma too bright: import content wrong";
}

TEST(DmaBufCudaImportTest, InvalidFdFallsBackWithoutCrash) {
  if (avcodec_find_encoder_by_name("h264_nvenc") == nullptr)
    GTEST_SKIP() << "h264_nvenc unavailable";
  FFmpegEncoder venc(IEncoder::EncoderType::NVENC_H264, "h264_nvenc");
  EncoderConfig vcfg;
  vcfg.width = 640;
  vcfg.height = 360;
  vcfg.fps = 30;
  vcfg.bitrate = 2000;
  if (!venc.initialize(vcfg))
    GTEST_SKIP() << "NVENC init failed (no GPU?)";

  VideoFrame frame{};
  frame.width = 640;
  frame.height = 360;
  frame.timestamp = 0;
  frame.pixelFormat = AV_PIX_FMT_DRM_PRIME;
  frame.isDmaBuf = true;
  frame.dmaBufFd = -1; // invalid: must not crash
  int fallbackRequests = 0;
  frame.requestCpuFallback = [&] { ++fallbackRequests; };
  frame.drmFormat = DRM_FORMAT_XRGB8888;
  frame.drmModifier = 0;
  frame.dmaBufStride = 640 * 4;
  frame.dmaBufOffset = 0;
  EncodedPacket pkt;
  EXPECT_FALSE(venc.encodeFrame(frame, pkt));
  EXPECT_GE(venc.droppedFrames(), 1);
  EXPECT_EQ(fallbackRequests, 1);

  // A valid fd which is not a DMA-BUF reaches the real EGL import failure.
  // Only that failure may announce checking CPU/SHM fallback.
  const int invalidBuffer = open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(invalidBuffer, 0);
  frame.dmaBufFd = invalidBuffer;
  ScopedEncoderMessages messages;
  EXPECT_FALSE(venc.encodeFrame(frame, pkt));
  close(invalidBuffer);
  EXPECT_EQ(fallbackRequests, 2);
  EXPECT_TRUE(messages.contains(QtDebugMsg, "trying EGL/CUDA import"));
  EXPECT_TRUE(messages.contains(QtWarningMsg, "EGL/CUDA import failed"));
  EXPECT_TRUE(messages.contains(QtWarningMsg, "checking CPU/SHM fallback"));
  EXPECT_FALSE(messages.contains(QtWarningMsg, "using CPU fallback"));
}

static void checkGpuResize(bool vaapi) {
  if (!vaapi && !eglImportUsable())
    GTEST_SKIP() << "no EGL device with dma-buf import";
  auto original = makeXrgbDmaBuf(640, 360, true);
  auto smaller = makeXrgbDmaBuf(320, 240, true);
  auto larger = makeXrgbDmaBuf(1280, 360, true);
  auto odd = makeXrgbDmaBuf(137, 79, true);
  if (!original || !smaller || !larger || !odd)
    GTEST_SKIP() << "no GBM XRGB DMA-BUF";
  if (!original->mappedFill || !smaller->mappedFill || !larger->mappedFill || !odd->mappedFill)
    GTEST_SKIP() << "GBM buffers cannot be filled for decoded-content checks";
  FFmpegEncoder encoder(vaapi ? IEncoder::EncoderType::VAAPI_H264 : IEncoder::EncoderType::NVENC_H264,
                         vaapi ? "h264_vaapi" : "h264_nvenc");
  EncoderConfig config;
  config.width = 640; config.height = 360; config.fps = 30; config.bitrate = 2000;
  if (!encoder.initialize(config))
    GTEST_SKIP() << (vaapi ? "VA-API unavailable" : "NVENC unavailable");

  int requests = 0;
  std::vector<EncodedPacket> packets;
  auto submit = [&](TestDmaBuf &buffer, int width, int height, int index) {
    VideoFrame frame{};
    frame.width = width; frame.height = height;
    frame.timestamp = int64_t(index) * 33333;
    frame.pixelFormat = AV_PIX_FMT_DRM_PRIME;
    frame.isDmaBuf = true;
    frame.dmaBufFd = buffer.fd;
    frame.dmaBufStride = buffer.stride;
    frame.drmFormat = DRM_FORMAT_XRGB8888;
    frame.drmModifier = buffer.modifier;
    frame.requestCpuFallback = [&] { ++requests; };
    EncodedPacket packet;
    if (encoder.encodeFrame(frame, packet))
      packets.push_back(std::move(packet));
  };
  struct Stage { TestDmaBuf *buffer; int width, height; };
  const Stage stages[] = {{original.get(), 640, 360}, {smaller.get(), 320, 240},
                           {larger.get(), 1280, 360}, {odd.get(), 137, 79},
                           {original.get(), 640, 360}};
  int submitted = 0;
  submit(*original, 640, 360, submitted++);
  if (vaapi && encoder.vaapiVppFrames() == 0 && requests > 0)
    GTEST_SKIP() << "VA-API driver cannot import/convert this XRGB DMA-BUF with VPP";
  for (auto stage : stages) {
    for (int i = 0; i < 6; ++i) {
      if (stage.buffer == original.get() && submitted == 1 && i == 0)
        continue;
      submit(*stage.buffer, stage.width, stage.height, submitted++);
    }
  }
  EXPECT_EQ(requests, 0);
  EXPECT_EQ(encoder.eglImportFrames(), vaapi ? 0 : submitted);
  EXPECT_EQ(encoder.vaapiVppFrames(), vaapi ? submitted : 0);
  EXPECT_EQ(encoder.gpuScaledFrames(), 18);
  EXPECT_EQ(encoder.droppedFrames(), 0);
  EXPECT_EQ(encoder.fallbackFrames(), 0);
  encoder.flush(packets);
  ASSERT_EQ(packets.size(), size_t(submitted));

  // Decode every frame, including transitions and the restored original
  // size. Four coloured quadrants prove scaling covers the full fitted area
  // in both axes and preserves channel order,
  // while black bars must replace (not retain) old pooled/staging pixels.
  struct Decoder {
    AVCodecContext *ctx = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    ~Decoder() {
      av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&ctx);
    }
  } decoder;
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  ASSERT_NE(codec, nullptr);
  decoder.ctx = avcodec_alloc_context3(codec);
  ASSERT_NE(decoder.ctx, nullptr);
  ASSERT_NE(decoder.frame, nullptr);
  ASSERT_NE(decoder.packet, nullptr);
  const auto extra = encoder.extraData();
  decoder.ctx->extradata = static_cast<uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  ASSERT_NE(decoder.ctx->extradata, nullptr);
  memcpy(decoder.ctx->extradata, extra.data(), extra.size());
  decoder.ctx->extradata_size = int(extra.size());
  ASSERT_EQ(avcodec_open2(decoder.ctx, codec, nullptr), 0);
  int decoded = 0;
  auto receive = [&] {
    int ret;
    while ((ret = avcodec_receive_frame(decoder.ctx, decoder.frame)) == 0) {
      ASSERT_LT(decoded, submitted);
      SCOPED_TRACE("frame " + std::to_string(decoded));
      ASSERT_EQ(decoder.frame->width, 640);
      ASSERT_EQ(decoder.frame->height, 360);
      const int stage = decoded / 6;
      int x = 0, y = 0, width = 640, height = 360;
      if (stage == 1) { x = 80; width = 480; }
      if (stage == 2) { y = 90; height = 180; }
      if (stage == 3) { x = 8; width = 624; }
      auto luma = [&](int px, int py) {
        return decoder.frame->data[0][py * decoder.frame->linesize[0] + px];
      };
      EXPECT_NEAR(luma(x + width / 4, y + height / 4), 81, 14); // red
      EXPECT_NEAR(luma(x + width * 3 / 4, y + height / 4), 145, 14); // green
      EXPECT_NEAR(luma(x + width / 4, y + height * 3 / 4), 41, 14); // blue
      EXPECT_NEAR(luma(x + width * 3 / 4, y + height * 3 / 4), 235, 14); // white
      if (stage == 1 || stage == 3) {
        EXPECT_LT(luma(2, 180), 26);
        EXPECT_LT(luma(637, 180), 26);
      }
      if (stage == 2) {
        EXPECT_LT(luma(320, 8), 26);
        EXPECT_LT(luma(320, 352), 26);
      }
      ++decoded;
      av_frame_unref(decoder.frame);
    }
    EXPECT_TRUE(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
  };
  for (auto &packet : packets) {
    ASSERT_EQ(av_new_packet(decoder.packet, int(packet.data.size())), 0);
    memcpy(decoder.packet->data, packet.data.data(), packet.data.size());
    ASSERT_EQ(avcodec_send_packet(decoder.ctx, decoder.packet), 0);
    av_packet_unref(decoder.packet);
    receive();
  }
  ASSERT_EQ(avcodec_send_packet(decoder.ctx, nullptr), 0);
  receive();
  EXPECT_EQ(decoded, submitted);
}

TEST(DmaBufCudaImportTest, RealResizedBuffersScaleOnGpuWithoutShmOrStalePixels) {
  checkGpuResize(false);
}

TEST(DmaBufVaapiImportTest, RealResizedBuffersConvertScaleAndPadOnGpuWhenSupported) {
#ifdef HAVE_VAAPI
  checkGpuResize(true);
#else
  GTEST_SKIP() << "VA-API VPP support not compiled";
#endif
}

namespace {
EncoderConfig lifetimeConfig() {
  EncoderConfig config;
  config.width = 640; config.height = 360; config.fps = 30; config.bitrate = 2000;
  return config;
}
void submitLifetimeFrame(FFmpegEncoder &encoder, TestDmaBuf &buffer, int timestamp,
                          int &fallbackRequests) {
  VideoFrame frame{};
  frame.width = 640; frame.height = 360; frame.timestamp = timestamp;
  frame.isDmaBuf = true; frame.pixelFormat = AV_PIX_FMT_DRM_PRIME;
  frame.dmaBufFd = buffer.fd; frame.dmaBufStride = buffer.stride;
  frame.drmFormat = DRM_FORMAT_XRGB8888; frame.drmModifier = buffer.modifier;
  frame.requestCpuFallback = [&] { ++fallbackRequests; };
  EncodedPacket packet;
  encoder.encodeFrame(frame, packet);
}
}

TEST(DmaBufCudaImportTest, ModifierQueryDoesNotInvalidateAnActiveImporter) {
  auto buffer = makeXrgbDmaBuf(640, 360);
  if (!buffer) GTEST_SKIP() << "GBM unavailable";
  FFmpegEncoder encoder(IEncoder::EncoderType::NVENC_H264, "h264_nvenc");
  if (!encoder.initialize(lifetimeConfig())) GTEST_SKIP() << "NVENC unavailable";
  int requests = 0;
  submitLifetimeFrame(encoder, *buffer, 0, requests);
  ASSERT_EQ(encoder.eglImportFrames(), 1);
  queryDmaBufModifiers(DRM_FORMAT_XRGB8888);
  submitLifetimeFrame(encoder, *buffer, 33333, requests);
  EXPECT_EQ(encoder.eglImportFrames(), 2);
  EXPECT_EQ(requests, 0);
  EXPECT_EQ(encoder.droppedFrames(), 0);
}

TEST(DmaBufCudaImportTest, RetiringOneImporterDoesNotInvalidateAnother) {
  auto buffer = makeXrgbDmaBuf(640, 360);
  if (!buffer) GTEST_SKIP() << "GBM unavailable";
  auto first = std::make_unique<FFmpegEncoder>(IEncoder::EncoderType::NVENC_H264, "h264_nvenc");
  FFmpegEncoder second(IEncoder::EncoderType::NVENC_H264, "h264_nvenc");
  if (!first->initialize(lifetimeConfig()) || !second.initialize(lifetimeConfig()))
    GTEST_SKIP() << "NVENC unavailable";
  int requests = 0;
  submitLifetimeFrame(*first, *buffer, 0, requests);
  ASSERT_EQ(first->eglImportFrames(), 1);
  submitLifetimeFrame(second, *buffer, 0, requests);
  ASSERT_EQ(second.eglImportFrames(), 1);
  first.reset();
  submitLifetimeFrame(second, *buffer, 33333, requests);
  EXPECT_EQ(second.eglImportFrames(), 2);
  EXPECT_EQ(requests, 0);
  EXPECT_EQ(second.droppedFrames(), 0);
}

TEST(DmaBufCudaImportTest, ConcurrentModifierQueriesKeepImportingWithoutFallback) {
  auto buffer = makeXrgbDmaBuf(640, 360);
  if (!buffer) GTEST_SKIP() << "GBM unavailable";
  FFmpegEncoder encoder(IEncoder::EncoderType::NVENC_H264, "h264_nvenc");
  if (!encoder.initialize(lifetimeConfig())) GTEST_SKIP() << "NVENC unavailable";
  int requests = 0;
  submitLifetimeFrame(encoder, *buffer, 0, requests);
  ASSERT_EQ(encoder.eglImportFrames(), 1);
  std::barrier start(2);
  std::atomic<int> completed{0};
  std::thread queries([&] {
    start.arrive_and_wait();
    for (int i = 0; i < 20; ++i) {
      queryDmaBufModifiers(DRM_FORMAT_XRGB8888);
      ++completed;
    }
  });
  start.arrive_and_wait();
  for (int i = 1; i <= 30; ++i)
    submitLifetimeFrame(encoder, *buffer, i * 33333, requests);
  queries.join();
  EXPECT_EQ(completed, 20);
  EXPECT_EQ(encoder.eglImportFrames(), 31);
  EXPECT_EQ(requests, 0);
  EXPECT_EQ(encoder.droppedFrames(), 0);
}
