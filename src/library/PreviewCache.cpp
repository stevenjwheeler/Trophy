/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PreviewCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QQuickImageProvider>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QThread>
#include <QtConcurrent>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace TrophyBackend {
struct PreviewFrames {
  struct Entry { QVector<QImage> images; qint64 bytes = 0; int pins = 0; };
  QMutex mutex;
  QHash<QString, Entry> entries;
  QList<QString> recent;
  qint64 bytes = 0;
  static constexpr qint64 budget = 48 * 1024 * 1024;

  int count(const QString &key) {
    QMutexLocker lock(&mutex);
    const auto it = entries.constFind(key);
    if (it == entries.cend()) return 0;
    recent.removeAll(key); recent.append(key);
    return it->images.size();
  }
  QImage frame(const QString &key, int index) {
    QMutexLocker lock(&mutex);
    const auto it = entries.constFind(key);
    if (it == entries.cend() || index < 0 || index >= it->images.size()) return {};
    return it->images.at(index); // implicit sharing, no pixel copy or file I/O
  }
  void pin(const QString &key, int delta) {
    QMutexLocker lock(&mutex);
    auto it = entries.find(key);
    if (it != entries.end()) it->pins = qMax(0, it->pins + delta);
  }
  void insert(const QString &key, QVector<QImage> images) {
    QVector<Entry> retired;
    {
      QMutexLocker lock(&mutex);
      if (entries.contains(key)) return;
      Entry entry{std::move(images)};
      for (const auto &image : entry.images) entry.bytes += image.sizeInBytes();
      bytes += entry.bytes;
      entries.insert(key, std::move(entry)); recent.append(key);
      for (int i = 0; bytes > budget && i < recent.size();) {
        const QString candidate = recent.at(i);
        if (candidate == key || entries[candidate].pins) { ++i; continue; }
        auto old = entries.take(candidate);
        bytes -= old.bytes;
        recent.removeAt(i);
        retired.append(std::move(old));
      }
    } // Free evicted images outside the image provider's short lookup lock.
  }
};

namespace {
constexpr int kFps = 15;
constexpr int kSeconds = 6;

class FrameProvider final : public QQuickImageProvider {
public:
  explicit FrameProvider(std::shared_ptr<PreviewFrames> frames)
      : QQuickImageProvider(QQuickImageProvider::Image), m_frames(std::move(frames)) {}
  QImage requestImage(const QString &id, QSize *size, const QSize &) override {
    const int slash = id.lastIndexOf('/');
    bool ok = false;
    const int index = id.mid(slash + 1).toInt(&ok);
    const QImage image = slash > 0 && ok ? m_frames->frame(id.left(slash), index) : QImage();
    if (size) *size = image.size();
    return image;
  }
private:
  std::shared_ptr<PreviewFrames> m_frames;
};

struct Job {
  std::shared_ptr<std::atomic_bool> cancel;
  QElapsedTimer elapsed;
  bool interrupted() const { return cancel->load() || elapsed.elapsed() > 30000; }
};

bool validCache(const QString &path) {
  const QFileInfo info(path);
  if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > 4 * 1024 * 1024)
    return false;
  AVFormatContext *ctx = nullptr;
  const QByteArray bytes = QFile::encodeName(path);
  bool ok = avformat_open_input(&ctx, bytes.constData(), nullptr, nullptr) >= 0;
  if (ok) {
    ok = avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->nb_streams == 1 &&
         ctx->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
         ctx->streams[0]->codecpar->codec_id == AV_CODEC_ID_MPEG4 &&
         ctx->streams[0]->codecpar->width > 0 && ctx->streams[0]->codecpar->width <= 320 &&
         ctx->streams[0]->codecpar->height > 0 && ctx->streams[0]->codecpar->height <= 180 &&
         ctx->duration > 0 && ctx->duration <= 7000000;
  }
  avformat_close_input(&ctx);
  return ok;
}

// All contexts and paths belong to this worker, never to a Recording QObject.
QString generate(const QString &source, const std::shared_ptr<std::atomic_bool> &cancel) {
  Job job{cancel, {}};
  job.elapsed.start();
  const QFileInfo original(source);
  if (!original.isFile() || original.size() <= 0 || job.interrupted())
    return {};
  const qint64 size = original.size();
  const qint64 mtime = original.lastModified().toMSecsSinceEpoch();
  const QByteArray fingerprint = QCryptographicHash::hash(
      QByteArray::number(size) + ':' + QByteArray::number(mtime), QCryptographicHash::Sha256).toHex().left(16);
  const QString prefix = original.completeBaseName() + ".preview-v1-" + original.suffix().toLower() + '-';
  const QDir directory(original.absolutePath() + "/.thumbnails");
  if (QFileInfo(directory.absolutePath()).isSymLink())
    return {};
  const QString cached = directory.filePath(prefix + QString::fromLatin1(fingerprint) + ".mp4");
  if (validCache(cached))
    return cached;
  if (!QDir().mkpath(directory.absolutePath()))
    return {};
  QTemporaryFile temporary(directory.filePath(prefix + "XXXXXX.part"));
  if (!temporary.open())
    return {};
  const QString tempPath = temporary.fileName();
  temporary.close();

  AVFormatContext *input = avformat_alloc_context();
  AVFormatContext *output = nullptr;
  AVCodecContext *decoder = nullptr;
  AVCodecContext *encoder = nullptr;
  AVFrame *frame = av_frame_alloc();
  AVFrame *scaled = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  AVPacket *encoded = av_packet_alloc();
  SwsContext *sws = nullptr;
  bool success = false;
  auto cleanup = [&] {
    sws_freeContext(sws);
    av_packet_free(&packet);
    av_packet_free(&encoded);
    av_frame_free(&frame);
    av_frame_free(&scaled);
    avcodec_free_context(&decoder);
    avcodec_free_context(&encoder);
    avformat_close_input(&input);
    if (output) {
      if (output->pb) avio_closep(&output->pb);
      avformat_free_context(output);
    }
  };
  auto work = [&]() -> bool {
    if (!input || !frame || !scaled || !packet || !encoded)
      return false;
    input->interrupt_callback = {[](void *opaque) -> int {
      return static_cast<Job *>(opaque)->interrupted();
    }, &job};
    const QByteArray inputName = QFile::encodeName(source);
    if (avformat_open_input(&input, inputName.constData(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(input, nullptr) < 0)
      return false;
    const int video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0 || job.interrupted()) return false;
    AVStream *stream = input->streams[video];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!decoder || avcodec_parameters_to_context(decoder, stream->codecpar) < 0)
      return false;
    decoder->thread_count = 2;
    if (avcodec_open2(decoder, codec, nullptr) < 0 || decoder->width <= 0 || decoder->height <= 0)
      return false;
    const double ratio = qMin(320.0 / decoder->width, 180.0 / decoder->height);
    const int width = qMax(2, int(decoder->width * ratio) & ~1);
    const int height = qMax(2, int(decoder->height * ratio) & ~1);
    const AVCodec *outCodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    encoder = outCodec ? avcodec_alloc_context3(outCodec) : nullptr;
    const QByteArray outputName = QFile::encodeName(tempPath);
    if (!encoder || avformat_alloc_output_context2(&output, nullptr, "mp4", outputName.constData()) < 0)
      return false;
    encoder->width = width;
    encoder->height = height;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = {1, kFps};
    encoder->framerate = {kFps, 1};
    encoder->bit_rate = 250000;
    encoder->gop_size = kFps;
    encoder->max_b_frames = 0;
    encoder->thread_count = 1;
    if (output->oformat->flags & AVFMT_GLOBALHEADER)
      encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(encoder, outCodec, nullptr) < 0) return false;
    AVStream *outStream = avformat_new_stream(output, nullptr);
    if (!outStream || avcodec_parameters_from_context(outStream->codecpar, encoder) < 0)
      return false;
    outStream->time_base = encoder->time_base;
    if (avio_open(&output->pb, outputName.constData(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(output, nullptr) < 0)
      return false;
    scaled->format = encoder->pix_fmt;
    scaled->width = width;
    scaled->height = height;
    if (av_frame_get_buffer(scaled, 32) < 0) return false;
    const int64_t origin = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    const double seconds = input->duration > 0 ? double(input->duration) / AV_TIME_BASE : 0;
    const double start = QRandomGenerator::global()->generateDouble() * qMax(0.0, seconds - kSeconds);
    const int64_t startMs = int64_t(start * 1000);
    if (startMs > 0 && av_seek_frame(input, video,
        av_sat_add64(origin, av_rescale_q(startMs, AVRational{1, 1000}, stream->time_base)), AVSEEK_FLAG_BACKWARD) < 0)
      return false;
    int64_t firstMs = -1;
    int64_t lastPts = -1;
    int count = 0;
    bool sampleDone = false;
    auto writePackets = [&]() {
      int result;
      while ((result = avcodec_receive_packet(encoder, encoded)) >= 0) {
        encoded->stream_index = outStream->index;
        av_packet_rescale_ts(encoded, encoder->time_base, outStream->time_base);
        const int written = av_interleaved_write_frame(output, encoded);
        av_packet_unref(encoded);
        if (written < 0) return false;
      }
      return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };
    auto receive = [&]() {
      int result;
      while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
        if (job.interrupted()) return false;
        const int64_t stamp = frame->best_effort_timestamp;
        if (stamp == AV_NOPTS_VALUE) { av_frame_unref(frame); continue; }
        const int64_t ms = av_rescale_q(av_sat_sub64(stamp, origin), stream->time_base, AVRational{1, 1000});
        if (ms < startMs) { av_frame_unref(frame); continue; }
        if (firstMs < 0) firstMs = ms; // tolerate sparse/gapped launch footage
        const int64_t pts = av_rescale_rnd(ms - firstMs, kFps, 1000, AV_ROUND_DOWN);
        if (pts >= kFps * kSeconds) { sampleDone = true; av_frame_unref(frame); return true; }
        if (pts <= lastPts) { av_frame_unref(frame); continue; }
        if (frame->width <= 0 || frame->height <= 0 || !frame->data[0] || frame->format < 0 ||
            av_image_check_size(frame->width, frame->height, 0, nullptr) < 0 ||
            av_frame_make_writable(scaled) < 0)
          return false;
        sws = sws_getCachedContext(sws, frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), width, height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws || sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
            scaled->data, scaled->linesize) != height)
          return false;
        scaled->pts = pts;
        if (avcodec_send_frame(encoder, scaled) < 0 || !writePackets()) return false;
        lastPts = pts;
        ++count;
        av_frame_unref(frame);
      }
      return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };
    int read = 0;
    int packets = 0;
    while (!job.interrupted() && !sampleDone && count < kFps * kSeconds &&
           (firstMs < 0 || lastPts < kFps * kSeconds - 1) &&
           (read = av_read_frame(input, packet)) >= 0) {
      if (++packets > 100000) return false;
      if (packet->stream_index == video) {
        const int sent = avcodec_send_packet(decoder, packet);
        if (sent < 0 || !receive()) return false;
      }
      av_packet_unref(packet);
    }
    if (job.interrupted() || (read < 0 && read != AVERROR_EOF)) return false;
    if (read == AVERROR_EOF && (avcodec_send_packet(decoder, nullptr) < 0 || !receive()))
      return false;
    if (count == 0 || avcodec_send_frame(encoder, nullptr) < 0 || !writePackets() ||
        av_write_trailer(output) < 0)
      return false;
    avio_flush(output->pb);
    const bool ioOk = output->pb->error >= 0;
    const int close = avio_closep(&output->pb);
    return ioOk && close >= 0;
  };
  success = work();
  cleanup();
  const QFileInfo current(source);
  if (!success || job.interrupted() || !current.isFile() || current.size() != size ||
      current.lastModified().toMSecsSinceEpoch() != mtime || !validCache(tempPath))
    return {};
  if (QFileInfo::exists(cached)) {
    if (validCache(cached)) return cached;
    if (QFileInfo(cached).isSymLink() || !QFile::remove(cached)) return {};
  }
  if (!QFile::rename(tempPath, cached)) return {};
  // Sweep only this video's obsolete version/fingerprint, not other sidecars.
  const QRegularExpression owned("^" + QRegularExpression::escape(prefix) + "[0-9a-f]{16}\\.mp4$");
  for (const QString &name : directory.entryList(QDir::Files | QDir::NoSymLinks)) {
    if (owned.match(name).hasMatch() && directory.filePath(name) != cached)
      QFile::remove(directory.filePath(name));
  }
  return cached;
}

PreviewResult loadFrames(const QString &cached, const std::shared_ptr<PreviewFrames> &store,
                         const std::shared_ptr<std::atomic_bool> &cancel) {
  if (cached.isEmpty() || cancel->load()) return {};
  const QFileInfo info(cached);
  const QString key = QString::fromLatin1(QCryptographicHash::hash(
      cached.toUtf8() + ':' + QByteArray::number(info.size()) + ':' +
      QByteArray::number(info.lastModified().toMSecsSinceEpoch()), QCryptographicHash::Sha256).toHex());
  if (const int count = store->count(key); count > 0) return {cached, key, count};
  Job job{cancel, {}}; job.elapsed.start();
  AVFormatContext *ctx = avformat_alloc_context();
  AVCodecContext *decoder = nullptr;
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  SwsContext *sws = nullptr;
  QVector<QImage> images;
  auto work = [&] {
    if (!ctx || !packet || !frame) return false;
    ctx->interrupt_callback = {[](void *opaque) -> int {
      return static_cast<Job *>(opaque)->interrupted();
    }, &job};
    if (avformat_open_input(&ctx, QFile::encodeName(cached).constData(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(ctx, nullptr) < 0 || ctx->nb_streams != 1)
      return false;
    const auto *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!decoder || avcodec_parameters_to_context(decoder, ctx->streams[0]->codecpar) < 0)
      return false;
    decoder->thread_count = 1;
    if (avcodec_open2(decoder, codec, nullptr) < 0) return false;
    int64_t first = AV_NOPTS_VALUE;
    auto receive = [&] {
      int result;
      while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
        if (job.interrupted() || frame->width <= 0 || frame->width > 320 ||
            frame->height <= 0 || frame->height > 180 || !frame->data[0]) return false;
        if (images.size() >= kFps * kSeconds) return true;
        QImage image(frame->width, frame->height, QImage::Format_RGB32);
        if (image.isNull()) return false;
        sws = sws_getCachedContext(sws, frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
            AV_PIX_FMT_RGB32, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        uint8_t *data[] = {image.bits(), nullptr, nullptr, nullptr};
        int strides[] = {int(image.bytesPerLine()), 0, 0, 0};
        if (!sws || sws_scale(sws, frame->data, frame->linesize, 0, frame->height, data, strides) != frame->height)
          return false;
        const auto stamp = frame->best_effort_timestamp;
        if (first == AV_NOPTS_VALUE) first = stamp;
        const int64_t index = stamp == AV_NOPTS_VALUE || first == AV_NOPTS_VALUE ? images.size() :
            av_rescale_q(av_sat_sub64(stamp, first), ctx->streams[0]->time_base, AVRational{1, kFps});
        while (!images.isEmpty() && images.size() < qMin<int64_t>(index, kFps * kSeconds))
          images.append(images.last()); // Preserve gaps without extra pixel allocations.
        if (images.size() < kFps * kSeconds) images.append(std::move(image));
        av_frame_unref(frame);
      }
      return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };
    int read = 0;
    while (!job.interrupted() && images.size() < kFps * kSeconds && (read = av_read_frame(ctx, packet)) >= 0) {
      if (packet->stream_index != 0 || avcodec_send_packet(decoder, packet) < 0 || !receive()) return false;
      av_packet_unref(packet);
    }
    if (job.interrupted() || (read < 0 && read != AVERROR_EOF)) return false;
    if (read == AVERROR_EOF && (avcodec_send_packet(decoder, nullptr) < 0 || !receive())) return false;
    return !images.isEmpty();
  };
  const bool ok = work();
  sws_freeContext(sws); av_frame_free(&frame); av_packet_free(&packet);
  avcodec_free_context(&decoder); avformat_close_input(&ctx);
  if (!ok || cancel->load()) {
    // A header-valid but undecodable cache must be regenerable on retry.
    if (!cancel->load() && !job.interrupted() && !QFileInfo(cached).isSymLink())
      QFile::remove(cached);
    return {};
  }
  const int count = images.size();
  store->insert(key, std::move(images));
  return {cached, key, count};
}
}

PreviewCache::PreviewCache(QObject *parent) : QObject(parent), m_cancel(std::make_shared<std::atomic_bool>(false)),
    m_frames(std::make_shared<PreviewFrames>()) {
  m_pool.setMaxThreadCount(1);
  m_pool.setThreadPriority(QThread::LowPriority);
  connect(&m_watcher, &QFutureWatcher<PreviewResult>::finished, this, [this] {
    const QString path = m_active;
    const PreviewResult result = m_watcher.result();
    const auto waiters = m_waiters.take(path);
    m_active.clear();
    m_completing = true;
    for (int ticket : waiters) {
      emit previewReady(QUrl::fromLocalFile(path), ticket,
          result.path.isEmpty() ? QUrl() : QUrl::fromLocalFile(result.path));
      emit previewFramesReady(QUrl::fromLocalFile(path), ticket,
          result.path.isEmpty() ? QUrl() : QUrl::fromLocalFile(result.path), result.key, result.count, kFps);
    }
    m_completing = false;
    startNext();
  });
}

PreviewCache::~PreviewCache() {
  m_cancel->store(true);
  m_watcher.disconnect(this);
  m_watcher.waitForFinished();
  m_pool.waitForDone();
}

void PreviewCache::requestPreview(const QUrl &source, int requestId) {
  cancelPreview(requestId);
  if (!source.isLocalFile()) {
    emit previewReady(source, requestId, {});
    emit previewFramesReady(source, requestId, {}, {}, 0, kFps);
    return;
  }
  const QString path = source.toLocalFile();
  if (!m_waiters.contains(path) && path != m_active) {
    if (m_queue.size() >= 64) {
      emit previewReady(source, requestId, {});
      emit previewFramesReady(source, requestId, {}, {}, 0, kFps);
      return;
    }
    m_queue.enqueue(path);
  }
  m_waiters[path].insert(requestId);
  startNext();
}

void PreviewCache::preparePreview(const QString &sourcePath) {
  // Foreground QML tickets are positive; post-save preparation is independent
  // of hover cancellation and joins the exact same pending/cache lookup.
  requestPreview(QUrl::fromLocalFile(sourcePath), m_backgroundTicket--);
}

void PreviewCache::cancelPreview(int requestId) {
  for (auto it = m_waiters.begin(); it != m_waiters.end();) {
    it->remove(requestId);
    if (it->isEmpty()) it = m_waiters.erase(it);
    else ++it;
  }
}

void PreviewCache::startNext() {
  if (!m_active.isEmpty() || m_completing) return;
  while (!m_queue.isEmpty()) {
    const QString path = m_queue.dequeue();
    if (!m_waiters.contains(path)) continue;
    m_active = path;
    const auto cancel = m_cancel;
    const auto frames = m_frames;
    m_watcher.setFuture(QtConcurrent::run(&m_pool, [path, cancel, frames] {
      return loadFrames(generate(path, cancel), frames, cancel);
    }));
    break;
  }
}

QQuickImageProvider *PreviewCache::createImageProvider() { return new FrameProvider(m_frames); }
void PreviewCache::retainFrames(const QString &key) { m_frames->pin(key, 1); }
void PreviewCache::releaseFrames(const QString &key) { m_frames->pin(key, -1); }
}
