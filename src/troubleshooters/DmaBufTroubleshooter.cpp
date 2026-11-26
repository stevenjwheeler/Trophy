/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DmaBufTroubleshooter.h"
#include "capture/WaylandPortalCapture.h"
#include "encoding/EncoderFactory.h"
#include "encoding/FFmpegEncoder.h"
#include <QMetaObject>
#include <chrono>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}

namespace TrophyBackend {
QVariantMap DmaBufTestEvidence::report() const {
  const bool passed = error.isEmpty() && accepted >= 3 && decoded == accepted &&
      singlePlane + multiPlane >= 3 && direct + egl + vpp >= 3 &&
      !fallback && !dropped && !shmFrames;
  QString status = passed ? QStringLiteral("passed") : QStringLiteral("inconclusive");
  QString summary = QObject::tr("Not enough DMA-BUF frames were verified. Retry with an animated window.");
  if (passed)
    summary = QObject::tr("DMA-BUF works for the selected source and tested encoder.");
  else if (!error.isEmpty()) {
    status = QStringLiteral("failed");
    summary = error;
  } else if (shmFrames || fallback) {
    status = QStringLiteral("fallback");
    summary = QObject::tr("Shared-memory fallback was observed. DMA-BUF was not verified for this selection. Try another source or check your graphics driver and portal.");
  } else if (dropped || (accepted && decoded != accepted)) {
    status = QStringLiteral("failed");
    summary = QObject::tr("The captured frames did not complete the GPU encode/decode check reliably.");
  }
  return {{"status", status}, {"summary", summary}, {"encoder", encoder},
          {"dmaFrames", dmaFrames}, {"shmFrames", shmFrames},
          {"singlePlane", singlePlane}, {"multiPlane", multiPlane},
          {"direct", direct}, {"egl", egl}, {"vpp", vpp},
          {"accepted", accepted}, {"decoded", decoded},
          {"fallback", fallback}, {"dropped", dropped}};
}

DmaBufTroubleshooter::DmaBufTroubleshooter(QObject *parent) : QObject(parent) {
  m_captureFactory = [] { return std::make_unique<WaylandPortalCapture>(); };
  m_deadline.setSingleShot(true);
  connect(&m_deadline, &QTimer::timeout, this, [this] { stop(); });
  m_poll.setInterval(25);
  connect(&m_poll, &QTimer::timeout, this, &DmaBufTroubleshooter::poll);
}

DmaBufTroubleshooter::~DmaBufTroubleshooter() {
  stop(tr("Test cancelled."));
  if (m_worker.valid()) m_worker.get();
  m_capture.reset(); // Native teardown and every retained frame have settled.
}

void DmaBufTroubleshooter::setAvailable(bool available) {
  if (m_available == available) return;
  m_available = available;
  if (!available) {
    m_cancelled = true;
    stop(tr("Test cancelled because recording or background capture started."));
  }
  emit changed();
}

void DmaBufTroubleshooter::start() {
  if (busy() || !available()) return;
  m_result.clear();
  m_stopReason.clear();
  m_cancelled = false;
  const auto generation = ++m_generation;
  m_stage = tr("Select an animated window or screen in the sharing dialog.");
  m_run = std::make_unique<Run>();
  m_capture = m_captureFactory();
  connect(m_capture.get(), &IVideoCaptureSource::captureError, this,
          [this, generation](const QString &error) {
    if (m_generation != generation || !m_capture) return;
    if (m_capture->lastErrorKind() == IVideoCaptureSource::CaptureErrorKind::Cancelled)
      m_cancelled = true;
    stop(error);
  });
  connect(m_capture.get(), &IVideoCaptureSource::captureStarted, this, [this, generation] {
    if (m_generation != generation || !m_run) return;
    {
      std::lock_guard lock(m_run->mutex);
      if (m_run->stopping) return;
    }
    m_stage = tr("Testing delivered DMA-BUF frames. Keep the selected content moving…");
    m_deadline.start(8000);
    emit changed();
  });
  m_capture->setFrameCallback([this](const VideoFrame &frame) { receive(frame); });
  m_worker = std::async(std::launch::async, [this, generation] {
    const auto complete = [this, generation] {
      QMetaObject::invokeMethod(this, [this, generation] {
        if (m_generation == generation) stop();
      }, Qt::QueuedConnection);
    };
    try {
      m_probe(*m_run, complete);
    } catch (...) {
      m_run->evidence.error = tr("The diagnostic worker could not complete.");
      complete();
    }
    // Never retire native capture before owner-thread prepareStopCapture().
    {
      std::unique_lock lock(m_run->mutex);
      m_run->wake.wait(lock, [this] { return m_run->stopping; });
      m_run->frames.clear();
    }
    m_capture->finishStopCapture();
  });
  m_poll.start();
  m_deadline.start(60000); // Includes the user's portal selection time.
  emit changed();
  {
    std::lock_guard lock(m_run->mutex);
    if (m_run->stopping) return;
  }
  CaptureConfig config;
  config.targetFps = 30;
  config.maxWidth = 1920;
  config.maxHeight = 1080;
  config.captureMouseCursor = false;
  config.persistPermission = false;
  // Fresh grant only; no restore tokens are read or saved.
  if (!m_capture->initialize(config) || !m_capture->startCapture())
    stop(tr("Could not start the diagnostic capture."));
}

void DmaBufTroubleshooter::receive(const VideoFrame &frame) {
  std::lock_guard lock(m_run->mutex);
  if (m_run->stopping) return;
  if (!frame.isDmaBuf) {
    ++m_run->evidence.shmFrames; // Never retain borrowed SHM pixels.
    return;
  }
  ++m_run->evidence.dmaFrames;
  // At most one queued producer buffer plus the one currently being encoded.
  if (!m_run->frames.empty()) return;
  VideoFrame retained = frame;
  if (!retained.retainDmaBuf()) return;
  m_run->frames.push_back(std::move(retained));
  m_run->wake.notify_one();
}

void DmaBufTroubleshooter::cancel() {
  if (!busy()) return;
  m_cancelled = true;
  stop(tr("Test cancelled."));
}

void DmaBufTroubleshooter::stop(const QString &reason) {
  if (!m_run) return;
  {
    std::lock_guard lock(m_run->mutex);
    if (m_run->stopping) return;
  }
  m_deadline.stop();
  m_stopReason = reason;
  m_stage = tr("Releasing capture and checking results…");
  m_capture->setFrameCallback({});
  m_capture->prepareStopCapture();
  {
    std::lock_guard lock(m_run->mutex);
    m_run->stopping = true;
  }
  m_run->wake.notify_one();
  emit changed();
}

void DmaBufTroubleshooter::poll() {
  if (!m_worker.valid() ||
      m_worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  m_worker.get();
  m_poll.stop();
  m_deadline.stop();
  m_result = m_run->evidence.report();
  if (!m_stopReason.isEmpty() || m_cancelled) {
    m_result["status"] = m_cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed");
    m_result["summary"] = m_stopReason.isEmpty() ? tr("Test cancelled.") : m_stopReason;
  }
  m_capture.reset();
  m_run.reset();
  m_stage = tr("Test complete");
  emit changed();
}

void DmaBufTroubleshooter::probe(Run &run, const std::function<void()> &complete) {
  std::unique_ptr<IEncoder> encoder;
  FFmpegEncoder *native = nullptr;
  struct Decoder {
    AVCodecContext *context = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    ~Decoder() {
      avcodec_free_context(&context);
      av_frame_free(&frame);
      av_packet_free(&packet);
    }
  } decoder;
  auto &e = run.evidence;
  auto drain = [&] {
    for (;;) {
      int rc = avcodec_receive_frame(decoder.context, decoder.frame);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
      if (rc < 0 || decoder.frame->decode_error_flags ||
          decoder.frame->width != encoder->width() ||
          decoder.frame->height != encoder->height()) return false;
      ++e.decoded;
      av_frame_unref(decoder.frame);
    }
  };
  auto decode = [&](const std::vector<EncodedPacket> &packets) {
    for (const auto &packet : packets) {
      if (packet.data.empty() || packet.data.size() > 32 * 1024 * 1024 ||
          av_new_packet(decoder.packet, int(packet.data.size())) < 0) return false;
      memcpy(decoder.packet->data, packet.data.data(), packet.data.size());
      const int rc = avcodec_send_packet(decoder.context, decoder.packet);
      av_packet_unref(decoder.packet);
      if (rc < 0 || !drain()) return false;
    }
    return true;
  };
  int submitted = 0;
  for (;;) {
    VideoFrame frame{};
    {
      std::unique_lock lock(run.mutex);
      run.wake.wait(lock, [&] { return run.stopping || !run.frames.empty(); });
      if (run.stopping) { run.frames.clear(); break; }
      frame = std::move(run.frames.front());
      run.frames.pop_front();
    }
    if (frame.width < 2 || frame.height < 2 ||
        frame.width > 8192 || frame.height > 4320) {
      e.error = tr("The selected source has unsupported dimensions.");
      break;
    }
    if (!encoder) {
      // Explicit hardware H.264 selection. Software success cannot pass this test.
      for (const auto &info : EncoderFactory::detectAvailableEncoders()) {
        if (info.type != IEncoder::EncoderType::NVENC_H264 &&
            info.type != IEncoder::EncoderType::VAAPI_H264) continue;
        auto candidate = EncoderFactory::create(info.type);
        EncoderConfig config;
        config.width = frame.width & ~1;
        config.height = frame.height & ~1;
        config.fps = 30;
        config.bitrate = 8000;
        if (candidate && candidate->initialize(config)) {
          encoder = std::move(candidate);
          break;
        }
      }
      if (!encoder) {
        e.error = tr("DMA-BUF was delivered, but no H.264 hardware encoder could be initialized. Check the graphics driver. Software encoding does not verify GPU import.");
        break;
      }
      native = dynamic_cast<FFmpegEncoder *>(encoder.get());
      e.encoder = encoder->name();
      QObject::connect(encoder.get(), &IEncoder::encodingError, encoder.get(),
                       [&e](const QString &error) { e.error = error; },
                       Qt::DirectConnection);
      decoder.context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
      if (!native || !decoder.context || !decoder.frame || !decoder.packet) {
        e.error = tr("Could not allocate the diagnostic decoder.");
        break;
      }
      const auto extra = encoder->extraData();
      decoder.context->extradata = static_cast<uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (!decoder.context->extradata) {
        e.error = tr("Could not allocate decoder headers.");
        break;
      }
      if (!extra.empty())
        memcpy(decoder.context->extradata, extra.data(), extra.size());
      decoder.context->extradata_size = int(extra.size());
      decoder.context->thread_count = 1;
      decoder.context->err_recognition = AV_EF_EXPLODE;
      if (avcodec_open2(decoder.context, nullptr, nullptr) < 0) {
        e.error = tr("Could not open the diagnostic decoder.");
        break;
      }
    }
    frame.timestamp = int64_t(submitted++) * 1000000 / 30;
    const auto before = native->zeroCopyFrames() + native->eglImportFrames() + native->vaapiVppFrames();
    std::vector<EncodedPacket> packets;
    encoder->encodeFrames(frame, packets);
    const auto after = native->zeroCopyFrames() + native->eglImportFrames() + native->vaapiVppFrames();
    if (after > before) {
      if (frame.dmaBufLayout().count > 1) ++e.multiPlane;
      else ++e.singlePlane;
    }
    if (!decode(packets)) {
      e.error = tr("The hardware encoder's output could not be decoded cleanly.");
      break;
    }
    if (submitted >= 90) break;
  }
  if (native) {
    if (e.error.isEmpty()) {
      std::vector<EncodedPacket> tail;
      if (encoder->flush(tail) < 0 || !decode(tail) ||
          avcodec_send_packet(decoder.context, nullptr) < 0 || !drain())
        e.error = tr("The diagnostic encoder/decoder could not finish cleanly.");
    }
    e.direct = native->zeroCopyFrames();
    e.egl = native->eglImportFrames();
    e.vpp = native->vaapiVppFrames();
    e.accepted = native->acceptedFrames();
    e.fallback = native->fallbackFrames();
    e.dropped = native->droppedFrames() + native->rejectedFrames();
  }
  complete();
}
} // namespace TrophyBackend
