/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "RecordingRecovery.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <array>
#include <vector>
#include <unistd.h>
#include <fcntl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace TrophyBackend {
namespace {
bool snapshot(const QString &path, struct stat &s) {
  return ::lstat(QFile::encodeName(path).constData(), &s) == 0 &&
         S_ISREG(s.st_mode) && s.st_nlink == 1 && s.st_uid == ::geteuid();
}
bool same(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
struct Input {
  AVFormatContext *ctx = nullptr;
  ~Input() { avformat_close_input(&ctx); }
};
struct Output {
  AVFormatContext *ctx = nullptr;
  ~Output() {
    if (ctx) {
      if (ctx->pb) avio_closep(&ctx->pb);
      avformat_free_context(ctx);
    }
  }
};
struct Packet {
  AVPacket *p = av_packet_alloc();
  ~Packet() { av_packet_free(&p); }
};
// Hash per stream, so muxer interleaving changes don't affect validation.
struct Contents {
  QCryptographicHash hash{QCryptographicHash::Sha256};
  qint64 count = 0;
  qint64 keys = 0;
  void add(const AVPacket *p, AVRational timeBase) {
    const qint64 size = p->size;
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(&size), sizeof(size)));
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(p->data), p->size));
    const qint64 pts = p->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
        av_rescale_q(p->pts, timeBase, AV_TIME_BASE_Q);
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(&pts), sizeof(pts)));
    keys += (p->flags & AV_PKT_FLAG_KEY) != 0;
    ++count;
  }
};
} // namespace

RecordingRecovery::~RecordingRecovery() {
  if (!m_temporary.isEmpty()) QFile::remove(m_temporary);
}

bool RecordingRecovery::prepare(const QString &path, const std::atomic_bool &cancel) {
  m_ready = false;
  m_error = QStringLiteral("Recovery did not complete; original preserved");
  m_source = path;
  if (!m_temporary.isEmpty() || cancel || !snapshot(path, m_original)) return false;
  // No subprocess/CLI dependency, no decode or re-encode. Leave room for the
  // original AND its replacement, including index growth and filesystem slack.
  const QStorageInfo disk(QFileInfo(path).absolutePath());
  if (!disk.isValid() || disk.bytesAvailable() < m_original.st_size + 64LL * 1024 * 1024)
    return false;
  Input in;
  in.ctx = avformat_alloc_context();
  if (!in.ctx) return false;
  in.ctx->interrupt_callback = {[](void *opaque) {
    return static_cast<const std::atomic_bool *>(opaque)->load() ? 1 : 0;
  }, const_cast<std::atomic_bool *>(&cancel)};
  if (avformat_open_input(&in.ctx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0)
    return false;
  const auto *kind = av_dict_get(in.ctx->metadata, "trophy-kind", nullptr, 0);
  const auto *version = av_dict_get(in.ctx->metadata, "trophy-version", nullptr, 0);
  if (!in.ctx->iformat || !QString::fromLatin1(in.ctx->iformat->name).contains("matroska") ||
      !kind || !version || in.ctx->nb_chapters || in.ctx->duration != AV_NOPTS_VALUE)
    return false;
  if (avformat_find_stream_info(in.ctx, nullptr) < 0 || cancel ||
      in.ctx->nb_streams == 0 || in.ctx->nb_streams > 32) return false;

  QTemporaryFile temporary(QFileInfo(path).absolutePath() + "/.trophy-recovery-XXXXXX.part");
  if (!temporary.open()) return false;
  m_temporary = temporary.fileName();
  temporary.setAutoRemove(false);
  temporary.close();
  Output out;
  if (avformat_alloc_output_context2(&out.ctx, nullptr, "matroska", QFile::encodeName(m_temporary).constData()) < 0 || !out.ctx)
    return false;
  out.ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
  if (av_dict_copy(&out.ctx->metadata, in.ctx->metadata, 0) < 0) return false;
  std::vector<std::unique_ptr<Contents>> original;
  bool video = false;
  for (unsigned i = 0; i < in.ctx->nb_streams; ++i) {
    const auto *s = in.ctx->streams[i];
    // Only Trophy's normal recording layout is repaired automatically.
    if (s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
      return false;
    video |= s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    AVStream *dest = avformat_new_stream(out.ctx, nullptr);
    if (!dest || avcodec_parameters_copy(dest->codecpar, s->codecpar) < 0 ||
        av_dict_copy(&dest->metadata, s->metadata, 0) < 0) return false;
    dest->time_base = s->time_base;
    dest->avg_frame_rate = s->avg_frame_rate;
    dest->disposition = s->disposition;
    original.push_back(std::make_unique<Contents>());
  }
  if (!video || avio_open(&out.ctx->pb, QFile::encodeName(m_temporary).constData(), AVIO_FLAG_WRITE) < 0 ||
      avformat_write_header(out.ctx, nullptr) < 0) return false;
  Packet packet;
  if (!packet.p) return false;
  int ret = AVERROR_EOF;
  while (!cancel && (ret = av_read_frame(in.ctx, packet.p)) >= 0) {
    const int i = packet.p->stream_index;
    if (i < 0 || unsigned(i) >= in.ctx->nb_streams || packet.p->size < 0 ||
        (packet.p->flags & AV_PKT_FLAG_CORRUPT)) return false;
    original[i]->add(packet.p, in.ctx->streams[i]->time_base);
    av_packet_rescale_ts(packet.p, in.ctx->streams[i]->time_base, out.ctx->streams[i]->time_base);
    packet.p->pos = -1;
    if (av_interleaved_write_frame(out.ctx, packet.p) < 0 || out.ctx->pb->error < 0) return false;
    av_packet_unref(packet.p);
  }
  // Matroska treats an interrupted final cluster as EOF; other read failures
  // aren't silently swallowed. Never publish empty or partly failed repairs.
  if (cancel || ret != AVERROR_EOF || av_write_trailer(out.ctx) < 0 || out.ctx->pb->error < 0)
    return false;
  if (avio_closep(&out.ctx->pb) < 0) return false;
  for (unsigned i = 0; i < in.ctx->nb_streams; ++i)
    if (in.ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !original[i]->keys)
      return false;

  Input check;
  if (avformat_open_input(&check.ctx, QFile::encodeName(m_temporary).constData(), nullptr, nullptr) < 0 ||
      check.ctx->duration <= 0 || check.ctx->duration == AV_NOPTS_VALUE ||
      check.ctx->nb_streams != original.size()) return false;
  std::vector<std::unique_ptr<Contents>> recovered;
  for (unsigned i = 0; i < check.ctx->nb_streams; ++i) {
    if (check.ctx->streams[i]->codecpar->codec_id != in.ctx->streams[i]->codecpar->codec_id)
      return false;
    recovered.push_back(std::make_unique<Contents>());
  }
  while (!cancel && (ret = av_read_frame(check.ctx, packet.p)) >= 0) {
    const int i = packet.p->stream_index;
    if (i < 0 || unsigned(i) >= recovered.size()) return false;
    recovered[i]->add(packet.p, check.ctx->streams[i]->time_base);
    av_packet_unref(packet.p);
  }
  if (cancel || ret != AVERROR_EOF) return false;
  for (unsigned i = 0; i < original.size(); ++i)
    if (original[i]->count != recovered[i]->count || original[i]->hash.result() != recovered[i]->hash.result())
      return false;
  if (av_seek_frame(check.ctx, -1, check.ctx->duration / 2, AVSEEK_FLAG_BACKWARD) < 0 ||
      av_read_frame(check.ctx, packet.p) < 0) return false;
  struct stat current {};
  if (!snapshot(path, current) || !same(m_original, current)) return false;
  // Flush the replacement before the atomic rename. A full disk or shutdown
  // during preparation leaves the original in place.
  const int fd = ::open(QFile::encodeName(m_temporary).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  m_ready = synced;
  return m_ready;
}

bool RecordingRecovery::publish() {
  struct stat current {};
  if (!m_ready || m_temporary.isEmpty() || !snapshot(m_source, current) || !same(m_original, current)) return false;
  if (::chmod(QFile::encodeName(m_temporary).constData(), m_original.st_mode & 0777) != 0) return false;
  const timespec times[] = {m_original.st_atim, m_original.st_mtim};
  if (::utimensat(AT_FDCWD, QFile::encodeName(m_temporary).constData(), times, AT_SYMLINK_NOFOLLOW) != 0)
    return false;
  if (::rename(QFile::encodeName(m_temporary).constData(), QFile::encodeName(m_source).constData()) != 0)
    return false;
  m_temporary.clear();
  return true;
}
} // namespace TrophyBackend
