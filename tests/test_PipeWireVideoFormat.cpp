/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <spa/pod/filter.h>
#include "capture/PipeWireVideoFormat.h"

using namespace TrophyBackend;

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
