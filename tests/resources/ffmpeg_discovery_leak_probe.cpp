// Standalone diagnostic: no Qt or Trophy objects, no frames encoded/captured.
// Mirrors the initial native operations used by EncoderFactory discovery.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include <cstring>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const bool both = std::strcmp(argv[1], "both") == 0;
  const bool nvenc = both || std::strcmp(argv[1], "nvenc") == 0;
  const bool vaapi = both || std::strcmp(argv[1], "vaapi") == 0;
  if (!nvenc && !vaapi)
    return 2;
  if (nvenc) {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec)
      return 77;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context)
      return 3;
    context->width = 1920;
    context->height = 1080;
    context->time_base = {1, 60};
    context->framerate = {60, 1};
    context->pix_fmt = AV_PIX_FMT_NV12;
    context->bit_rate = 6000000;
    avcodec_open2(context, codec, nullptr);
    avcodec_free_context(&context);
  }
  if (vaapi) {
    AVBufferRef *device = nullptr;
    av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    av_buffer_unref(&device);
  }
  return 0;
}
