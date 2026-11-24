/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <vector>
#include <climits>
#include "DmaBufFrame.h"
#include <spa/buffer/buffer.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

namespace TrophyBackend {

// Packed BGRx SHM consumers copy complete rows, including their padding.
// Validate before exposing a pointer: neither chunk metadata nor negotiated
// dimensions prove that the producer supplied that many mapped bytes.
inline const uint8_t *pipeWireVideoShmData(const spa_data &data,
                                         int width, int height) {
  if ((data.type != SPA_DATA_MemFd && data.type != SPA_DATA_MemPtr) ||
      !data.data || !data.chunk || width <= 0 || height <= 0 ||
      (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
      data.chunk->stride <= 0)
    return nullptr;
  const uint64_t stride = data.chunk->stride;
  const uint64_t bytes = stride * uint64_t(height);
  const uint64_t offset = data.chunk->offset;
  if (stride < uint64_t(width) * 4 || offset > data.maxsize ||
      bytes > uint64_t(data.maxsize) - offset || bytes > data.chunk->size)
    return nullptr;
  return static_cast<const uint8_t *>(data.data) + offset;
}

// SPA names describe byte order, whereas DRM XRGB8888 describes a packed
// integer. On little-endian Linux its bytes are B, G, R, X, not X, R, G, B.
// Use the same format for DMA-BUF and SHM; the modifier property's presence
// distinguishes the two transports (LINEAR=0 is still a real modifier).
inline const spa_pod *buildPipeWireVideoFormat(
    spa_pod_builder *builder, const std::vector<uint64_t> &modifiers = {}) {
  spa_pod_frame object;
  spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(builder,
                      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                      SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
                      0);
  if (!modifiers.empty()) {
    spa_pod_frame choice;
    // Let the producer test allocation with the entire intersection instead
    // of PipeWire prematurely fixing a modifier the compositor cannot use.
    spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier,
                         SPA_POD_PROP_FLAG_MANDATORY |
                             SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
    spa_pod_builder_long(builder, static_cast<int64_t>(modifiers.front()));
    for (uint64_t modifier : modifiers)
      spa_pod_builder_long(builder, static_cast<int64_t>(modifier));
    spa_pod_builder_pop(builder, &choice);
  }
  return static_cast<const spa_pod *>(spa_pod_builder_pop(builder, &object));
}

inline bool pipeWireVideoHasModifier(const spa_pod *format) {
  return format && spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_modifier);
}

inline uint32_t pipeWireVideoBufferTypes(const spa_pod *format, bool dmaBufEnabled) {
  return dmaBufEnabled && pipeWireVideoHasModifier(format)
             ? (1u << SPA_DATA_DmaBuf)
             : (1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr);
}

inline const spa_pod *buildPipeWireBufferRequest(
    spa_pod_builder *builder, const spa_pod *format, bool dmaBufEnabled) {
  spa_pod_frame object;
  spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
  spa_pod_builder_add(builder,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 32),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
          pipeWireVideoBufferTypes(format, dmaBufEnabled)), 0);
  // The producer knows how many image/auxiliary planes its modifier needs.
  // SHM remains exactly one contiguous pixel block.
  if (!dmaBufEnabled || !pipeWireVideoHasModifier(format))
    spa_pod_builder_add(builder, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), 0);
  return static_cast<const spa_pod *>(spa_pod_builder_pop(builder, &object));
}

inline bool pipeWireVideoDmaBufLayout(const spa_buffer &buffer, DmaBufLayout &out) {
  out = {};
  if (!buffer.datas || !buffer.n_datas || buffer.n_datas > DmaBufLayout::maxPlanes)
    return false;
  DmaBufLayout result;
  for (uint32_t i = 0; i < buffer.n_datas; ++i) {
    const auto &data = buffer.datas[i];
    // Sync objects are not image planes. Explicit-sync capture is not
    // negotiated/supported here; do not silently drop these extra blocks.
    if (data.type != SPA_DATA_DmaBuf || data.fd < 0 || data.fd > INT_MAX ||
        data.mapoffset != 0 || !data.chunk ||
        (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) || data.chunk->stride <= 0 ||
        (i == 0 && data.chunk->offset > INT_MAX))
      return false;
    // DMA-BUF exporters may use maxsize=0. Offsets describe the imported
    // allocation, not a mapped SHM ring; do not modulo by maxsize.
    result.planes[i] = {int(data.fd), data.chunk->stride, data.chunk->offset, data.maxsize};
    ++result.count;
  }
  out = result;
  return true;
}

} // namespace TrophyBackend
