/*
 * Trophy - Unit Tests
 * Stage B: feed a real GBM-exported XRGB DMA-BUF through FFmpegEncoder /
 * NVENC and assert the EGL/CUDA import path (not hwmap, not CPU fallback)
 * takes it. Skips gracefully without NVIDIA EGL / GBM / NVENC.
 */

#include <QCoreApplication>
#include <QTemporaryDir>
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

#include "capture/IVideoCaptureSource.h"
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
    if (eglInitialize(dpy, nullptr, nullptr) == EGL_TRUE) {
      const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
      ok = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
      eglTerminate(dpy);
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
    if (bo)
      gbm_bo_destroy(bo);
    if (gbm)
      gbm_device_destroy(gbm);
    if (drmFd >= 0)
      close(drmFd);
  }
};

// Horizontal gradient so decoded luma has a known mean (~mid-grey).
static std::unique_ptr<TestDmaBuf> makeXrgbDmaBuf(int w, int h) {
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
        row[x * 4 + 0] = v;
        row[x * 4 + 1] = v;
        row[x * 4 + 2] = v;
        row[x * 4 + 3] = 0xFF;
      }
    }
    gbm_bo_unmap(buf->bo, mapData);
    buf->mappedFill = true;
  }
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
  frame.drmFormat = DRM_FORMAT_XRGB8888;
  frame.drmModifier = 0;
  frame.dmaBufStride = 640 * 4;
  frame.dmaBufOffset = 0;
  EncodedPacket pkt;
  EXPECT_FALSE(venc.encodeFrame(frame, pkt));
  EXPECT_GE(venc.droppedFrames(), 1);
}
