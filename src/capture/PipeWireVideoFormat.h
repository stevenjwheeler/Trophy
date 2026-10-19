/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <vector>
#include <spa/buffer/buffer.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

namespace TrophyBackend {

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

} // namespace TrophyBackend
