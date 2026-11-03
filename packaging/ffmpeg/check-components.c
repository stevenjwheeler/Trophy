/*
 * Component check for the slim FFmpeg the AppImage ships.
 *
 * Trophy and Qt Multimedia only need a bounded part of FFmpeg. When the build
 * is slimmed (packaging/ffmpeg/build-ffmpeg.sh) an accidentally missing
 * encoder, muxer or protocol only shows up as a runtime failure in recording,
 * share export, hover previews or playback, so verify the list here.
 *
 * Hardware encoder *presence* is checked without opening a device: finding the
 * codec does not touch the GPU, and availability is still probed at runtime.
 *
 * Build and run:
 *   cc check-components.c \
 *      $(pkg-config --cflags --libs libavcodec libavformat libavutil) \
 *      -o check-components && ./check-components
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void need_encoder(const char *name) {
    if (!avcodec_find_encoder_by_name(name)) {
        fprintf(stderr, "missing encoder: %s\n", name);
        failures++;
    }
}

static void need_decoder(const char *name) {
    if (!avcodec_find_decoder_by_name(name)) {
        fprintf(stderr, "missing decoder: %s\n", name);
        failures++;
    }
}

static void need_muxer(const char *name) {
    if (!av_guess_format(name, NULL, NULL)) {
        fprintf(stderr, "missing muxer: %s\n", name);
        failures++;
    }
}

static void need_demuxer(const char *name) {
    /* av_find_input_format also matches the comma-separated aliases real
     * demuxers use ("matroska,webm", "mov,mp4,m4a,..."). */
    if (!av_find_input_format(name)) {
        fprintf(stderr, "missing demuxer: %s\n", name);
        failures++;
    }
}

int main(void) {
    /* Recording/DVR/export: software, NVENC and VA-API H.264, plus the
     * selectable HEVC/AV1 hardware paths. */
    need_encoder("libx264");
    need_encoder("h264_nvenc");
    need_encoder("hevc_nvenc");
    need_encoder("av1_nvenc");
    need_encoder("h264_vaapi");
    need_encoder("hevc_vaapi");
    need_encoder("av1_vaapi");
    /* Audio tracks (native AAC, libopus because the native Opus encoder is
     * still experimental) and the hover-preview MPEG-4 encoder. */
    need_encoder("aac");
    need_encoder("libopus");
    need_encoder("mpeg4");

    /* Decoding our own recordings, exports and previews. */
    need_decoder("h264");
    need_decoder("hevc");
    need_decoder("av1");
    need_decoder("aac");
    need_decoder("opus");
    need_decoder("mpeg4");

    need_muxer("matroska");
    need_muxer("mp4");
    need_demuxer("matroska");
    need_demuxer("mov");

    /* Local files only: --disable-network is intentional, file:// is not. */
    int have_file = 0;
    void *opaque = NULL;
    const char *protocol;
    while ((protocol = avio_enum_protocols(&opaque, 0))) {
        if (strcmp(protocol, "file") == 0)
            have_file = 1;
    }
    if (!have_file) {
        fprintf(stderr, "missing input protocol: file\n");
        failures++;
    }

    if (failures) {
        fprintf(stderr, "%d required FFmpeg component(s) missing\n", failures);
        return 1;
    }
    printf("FFmpeg %s: all required components present\n", av_version_info());
    return 0;
}
