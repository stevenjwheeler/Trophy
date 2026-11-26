/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <spa/pod/filter.h>
#include "capture/PipeWireVideoFormat.h"

using namespace TrophyBackend;

TEST(PipeWireVideoFormatTest, ShmHonorsOffsetAndRejectsTruncatedOrInvalidRows) {
  uint8_t pixels[80]{};
  spa_chunk chunk{};
  chunk.offset = 16;
  chunk.size = 64;
  chunk.stride = 32;
  spa_data data{};
  data.type = SPA_DATA_MemFd;
  data.data = pixels;
  data.maxsize = sizeof(pixels);
  data.chunk = &chunk;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), pixels + 16);
  chunk.size = 63;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), nullptr);
  chunk.size = 64;
  data.maxsize = 79;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), nullptr);
  data.maxsize = 80;
  chunk.offset = UINT32_MAX;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), nullptr);
  chunk.offset = 16;
  EXPECT_EQ(pipeWireVideoShmData(data, 9, 2), nullptr);
  EXPECT_EQ(pipeWireVideoShmData(data, 7, INT32_MAX), nullptr);
  EXPECT_EQ(pipeWireVideoShmData(data, 0, 2), nullptr);
  chunk.stride = -32;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), nullptr);
  chunk.stride = 32;
  chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), nullptr);
  chunk.flags = 0;
  data.type = SPA_DATA_DmaBuf;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), nullptr);
  data.type = SPA_DATA_MemPtr;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), pixels + 16);
  data.chunk = nullptr;
  EXPECT_EQ(pipeWireVideoShmData(data, 7, 2), nullptr);
}

TEST(PipeWireVideoFormatTest, DrmXrgbUsesBgrxAndProducerFixation) {
  uint8_t storage[4096];
  auto builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  const std::vector<uint64_t> modifiers{0x0300000000606010ULL, 0};
  const auto *format = buildPipeWireVideoFormat(&builder, modifiers);
  ASSERT_NE(format, nullptr);
  spa_video_info_raw info{};
  ASSERT_GE(spa_format_video_raw_parse(format, &info), 0);
  EXPECT_EQ(info.format, SPA_VIDEO_FORMAT_BGRx);
  const auto *prop = spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_modifier);
  ASSERT_NE(prop, nullptr);
  EXPECT_EQ(prop->flags & (SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE),
            SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
  EXPECT_EQ(SPA_POD_CHOICE_TYPE(&prop->value), SPA_CHOICE_Enum);
  ASSERT_EQ(SPA_POD_CHOICE_N_VALUES(&prop->value), 3u);
  const auto *values = static_cast<const int64_t *>(SPA_POD_CHOICE_VALUES(&prop->value));
  EXPECT_EQ(static_cast<uint64_t>(values[0]), modifiers[0]);
  EXPECT_EQ(static_cast<uint64_t>(values[1]), modifiers[0]);
  EXPECT_EQ(values[2], 0);
  EXPECT_EQ(pipeWireVideoBufferTypes(format, true), 1u << SPA_DATA_DmaBuf);
}

TEST(PipeWireVideoFormatTest, NoEglCapabilitiesMeansSharedMemoryOnly) {
  uint8_t storage[4096];
  auto builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  const auto *format = buildPipeWireVideoFormat(&builder);
  ASSERT_NE(format, nullptr);
  EXPECT_FALSE(pipeWireVideoHasModifier(format));
  EXPECT_EQ(pipeWireVideoBufferTypes(format, true),
            (1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr));
}

TEST(PipeWireVideoFormatTest, LinearAndImplicitModifiersAreNotSharedMemory) {
  for (int64_t modifier : {int64_t(0), int64_t(0x00ffffffffffffffULL)}) {
    uint8_t storage[4096];
    auto builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const auto *format = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_modifier, SPA_POD_Long(modifier)));
    EXPECT_TRUE(pipeWireVideoHasModifier(format));
    EXPECT_EQ(pipeWireVideoBufferTypes(format, true), 1u << SPA_DATA_DmaBuf);
    EXPECT_EQ(pipeWireVideoBufferTypes(format, false),
              (1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr));
  }
}

TEST(PipeWireVideoFormatTest, FullModifierListFitsAndIntersectsProducer) {
  uint8_t storage[4096];
  auto builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  std::vector<uint64_t> modifiers;
  for (uint64_t i = 1; i <= 64; ++i)
    modifiers.push_back(i);
  const auto *consumer = buildPipeWireVideoFormat(&builder, modifiers);
  ASSERT_NE(consumer, nullptr);
  const auto *producer = buildPipeWireVideoFormat(&builder, {64});
  ASSERT_NE(producer, nullptr);
  uint8_t filteredStorage[4096];
  auto filterBuilder = SPA_POD_BUILDER_INIT(filteredStorage, sizeof(filteredStorage));
  spa_pod *result = nullptr;
  ASSERT_EQ(spa_pod_filter(&filterBuilder, &result, producer, consumer), 0);
  ASSERT_NE(result, nullptr);
  const auto *prop = spa_pod_find_prop(result, nullptr, SPA_FORMAT_VIDEO_modifier);
  ASSERT_NE(prop, nullptr);
  const auto *values = static_cast<const int64_t *>(SPA_POD_CHOICE_VALUES(&prop->value));
  EXPECT_EQ(values[0], 64);
}

TEST(PipeWireVideoFormatTest, ModifierOfferCannotAccidentallyMatchSharedMemory) {
  uint8_t storage[4096];
  auto builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  const auto *dma = buildPipeWireVideoFormat(&builder, {0});
  const auto *shm = buildPipeWireVideoFormat(&builder);
  uint8_t resultStorage[4096];
  auto resultBuilder = SPA_POD_BUILDER_INIT(resultStorage, sizeof(resultStorage));
  spa_pod *result = nullptr;
  EXPECT_LT(spa_pod_filter(&resultBuilder, &result, shm, dma), 0);
}

TEST(PipeWireVideoFormatTest, ProducerChoosesDmaBufPlaneCountButShmStillRequiresOneBlock) {
  uint8_t storage[4096]{};
  auto builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  const auto *format = buildPipeWireVideoFormat(&builder, {0});
  const auto *dma = buildPipeWireBufferRequest(&builder, format, true);
  ASSERT_NE(dma, nullptr);
  EXPECT_EQ(spa_pod_find_prop(dma, nullptr, SPA_PARAM_BUFFERS_blocks), nullptr);
  const auto *shm = buildPipeWireBufferRequest(&builder, format, false);
  ASSERT_NE(shm, nullptr);
  const auto *blocks = spa_pod_find_prop(shm, nullptr, SPA_PARAM_BUFFERS_blocks);
  ASSERT_NE(blocks, nullptr);
  int32_t count = 0;
  ASSERT_EQ(spa_pod_get_int(&blocks->value, &count), 0);
  EXPECT_EQ(count, 1);
}

TEST(PipeWireVideoFormatTest, PreservesImageAndAuxiliaryPlanesWithoutAssumingMaxsize) {
  spa_chunk chunks[4]{};
  spa_data data[4]{};
  for (int i = 0; i < 4; ++i) {
    chunks[i].offset = i * 4096;
    chunks[i].stride = 256 >> i;
    data[i].type = SPA_DATA_DmaBuf;
    data[i].fd = 10 + i;
    data[i].chunk = &chunks[i];
  }
  spa_buffer buffer{};
  buffer.datas = data;
  DmaBufLayout layout;
  for (const auto count : {1u, 2u, 4u}) {
    buffer.n_datas = count;
    ASSERT_TRUE(pipeWireVideoDmaBufLayout(buffer, layout));
    ASSERT_EQ(layout.count, count);
    for (uint32_t i = 0; i < count; ++i) {
      EXPECT_EQ(layout.planes[i].fd, data[i].fd);
      EXPECT_EQ(layout.planes[i].offset, chunks[i].offset);
      EXPECT_EQ(layout.planes[i].stride, chunks[i].stride);
      EXPECT_EQ(layout.planes[i].size, 0u);
    }
  }
  buffer.n_datas = 0;
  EXPECT_FALSE(pipeWireVideoDmaBufLayout(buffer, layout));
  EXPECT_EQ(layout.count, 0u);
  buffer.n_datas = 5;
  EXPECT_FALSE(pipeWireVideoDmaBufLayout(buffer, layout));
}

TEST(PipeWireVideoFormatTest, RejectsMalformedMixedAndExplicitSyncBlocksWithoutPartialOutput) {
  spa_chunk chunks[2]{};
  chunks[0].stride = chunks[1].stride = 256;
  spa_data data[2]{};
  data[0].type = data[1].type = SPA_DATA_DmaBuf;
  data[0].fd = data[1].fd = 10;
  data[0].chunk = &chunks[0]; data[1].chunk = &chunks[1];
  spa_buffer buffer{};
  buffer.n_datas = 2; buffer.datas = data;
  const spa_data original = data[1];
  for (int kind = 0; kind < 7; ++kind) {
    SCOPED_TRACE(kind);
    data[1] = original; chunks[1].flags = 0; chunks[1].stride = 256;
    switch (kind) {
    case 0: data[1].type = SPA_DATA_MemFd; break;
    case 1: data[1].type = 5; break; // SPA_DATA_SyncObj tag, absent from older SPA headers.
    case 2: data[1].fd = int64_t(INT_MAX) + 1; break;
    case 3: data[1].fd = -1; break;
    case 4: data[1].mapoffset = 4096; break;
    case 5: data[1].chunk = nullptr; break;
    case 6: chunks[1].flags = SPA_CHUNK_FLAG_CORRUPTED; break;
    }
    DmaBufLayout layout;
    EXPECT_FALSE(pipeWireVideoDmaBufLayout(buffer, layout));
    EXPECT_EQ(layout.count, 0u);
  }
}
