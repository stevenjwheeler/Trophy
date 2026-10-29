#include "encoding/VaapiVideoProcessor.h"
#include <gtest/gtest.h>
#include <string>

using namespace TrophyBackend;

namespace {
struct FakeVa {
  static thread_local FakeVa *current;
  std::vector<std::string> calls;
  std::string failAt;
  VASurfaceID failSource = VA_INVALID_SURFACE;
  std::vector<VAProcColorStandardType> driverColors;
  VARectangle source{}, fit{};
  VAProcPipelineParameterBuffer params{};
  struct Submission {
    VAProcPipelineParameterBuffer params;
    VARectangle source, fit;
  };
  std::vector<Submission> submissions;
  bool bt601 = true;
  uint32_t maxInput = 8192;
  FakeVa() { current = this; }
  ~FakeVa() { current = nullptr; }
  static VAStatus call(const char *name) {
    current->calls.emplace_back(name);
    return current->failAt == name &&
                   (current->failSource == VA_INVALID_SURFACE ||
                    current->params.surface == current->failSource)
               ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
  }
  static VaapiVideoProcessor::Ops ops() {
    VaapiVideoProcessor::Ops ops;
    ops.createConfig = [](VADisplay, VAProfile profile, VAEntrypoint entry,
                           VAConfigAttrib *, int, VAConfigID *id) {
      EXPECT_EQ(profile, VAProfileNone); EXPECT_EQ(entry, VAEntrypointVideoProc);
      auto status = call("config"); if (!status) *id = 11; return status;
    };
    ops.destroyConfig = [](VADisplay, VAConfigID id) {
      EXPECT_EQ(id, 11u); return call("destroy-config");
    };
    ops.createContext = [](VADisplay, VAConfigID id, int width, int height, int,
                            VASurfaceID *targets, int count, VAContextID *context) {
      EXPECT_EQ(id, 11u); EXPECT_GT(width, 0); EXPECT_GT(height, 0);
      EXPECT_EQ(count, 2); EXPECT_EQ(targets[0], 101u); EXPECT_EQ(targets[1], 102u);
      auto status = call("context"); if (!status) *context = 12; return status;
    };
    ops.destroyContext = [](VADisplay, VAContextID id) {
      EXPECT_EQ(id, 12u); return call("destroy-context");
    };
    ops.queryCaps = [](VADisplay, VAContextID id, VABufferID *, unsigned, VAProcPipelineCaps *caps) {
      EXPECT_EQ(id, 12u);
      caps->max_input_width = caps->max_input_height = current->maxInput;
      caps->max_output_width = caps->max_output_height = 8192;
      caps->output_color_standards[0] = current->bt601 ? VAProcColorStandardBT601 : VAProcColorStandardBT709;
      caps->num_output_color_standards = 1;
      return call("caps");
    };
    ops.createBuffer = [](VADisplay, VAContextID id, VABufferType type,
                           unsigned size, unsigned count, void *data, VABufferID *buffer) {
      EXPECT_EQ(id, 12u); EXPECT_EQ(type, VAProcPipelineParameterBufferType);
      EXPECT_EQ(size, sizeof(VAProcPipelineParameterBuffer)); EXPECT_EQ(count, 1u);
      current->params = *static_cast<VAProcPipelineParameterBuffer *>(data);
      current->source = *current->params.surface_region;
      current->fit = *current->params.output_region;
      // No borrowed stack pointers escape the fake native operation.
      current->params.surface_region = current->params.output_region = nullptr;
      current->submissions.push_back({current->params, current->source, current->fit});
      auto status = call("buffer"); if (!status) *buffer = 13; return status;
    };
    ops.destroyBuffer = [](VADisplay, VABufferID id) {
      EXPECT_EQ(id, 13u); return call("destroy-buffer");
    };
    ops.begin = [](VADisplay, VAContextID id, VASurfaceID target) {
      EXPECT_EQ(id, 12u); EXPECT_EQ(target, 101u); return call("begin");
    };
    ops.render = [](VADisplay, VAContextID id, VABufferID *buffer, int count) {
      EXPECT_EQ(id, 12u); EXPECT_EQ(*buffer, 13u); EXPECT_EQ(count, 1); return call("render");
    };
    ops.end = [](VADisplay, VAContextID id) {
      EXPECT_EQ(id, 12u); return call("end");
    };
    ops.sync = [](VADisplay, VASurfaceID id) { return call(id == 101 ? "sync-output" : "sync-source"); };
    return ops;
  }
};
thread_local FakeVa *FakeVa::current = nullptr;
const auto display = reinterpret_cast<VADisplay>(uintptr_t(1));
const std::vector<VASurfaceID> targets{101, 102};
}

TEST(VaapiVideoProcessorTest, FitsAndConvertsWithBlackPaddingAndSynchronizesBeforeCleanup) {
  FakeVa fake;
  {
    VaapiVideoProcessor processor(FakeVa::ops());
    ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
    ASSERT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
    ASSERT_EQ(fake.submissions.size(), 2u);
    const auto &clear = fake.submissions.front();
    EXPECT_EQ(clear.params.surface, 98u);
    EXPECT_EQ(clear.source.width, 640); EXPECT_EQ(clear.source.height, 360);
    EXPECT_EQ(clear.fit.x, 0); EXPECT_EQ(clear.fit.y, 0);
    EXPECT_EQ(clear.fit.width, 640); EXPECT_EQ(clear.fit.height, 360);
    EXPECT_EQ(clear.params.surface_color_standard, VAProcColorStandardBT601);
    EXPECT_EQ(clear.params.input_color_properties.color_range, VA_SOURCE_RANGE_REDUCED);
    EXPECT_EQ(clear.params.output_color_properties.color_range, VA_SOURCE_RANGE_REDUCED);
    EXPECT_EQ(fake.params.surface, 99u);
    EXPECT_EQ(fake.source.width, 320); EXPECT_EQ(fake.source.height, 240);
    EXPECT_EQ(fake.fit.x, 80); EXPECT_EQ(fake.fit.y, 0);
    EXPECT_EQ(fake.fit.width, 480); EXPECT_EQ(fake.fit.height, 360);
    EXPECT_EQ(fake.params.output_background_color, 0xff000000u);
    EXPECT_EQ(fake.params.output_color_standard, VAProcColorStandardBT601);
    EXPECT_EQ(fake.params.input_color_properties.color_range, VA_SOURCE_RANGE_FULL);
    EXPECT_EQ(fake.params.output_color_properties.color_range, VA_SOURCE_RANGE_REDUCED);
    EXPECT_EQ(fake.calls, (std::vector<std::string>{"config", "context", "caps", "buffer",
                                                 "begin", "render", "end", "sync-output", "destroy-buffer", "buffer",
                                                 "begin", "render", "end", "sync-output", "destroy-buffer"}));
  }
  EXPECT_EQ(fake.calls[fake.calls.size() - 2], "destroy-context");
  EXPECT_EQ(fake.calls.back(), "destroy-config");
}

TEST(VaapiVideoProcessorTest, ReusesOutputContextAcrossInputResizesAndRebuildsForReplacement) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  EXPECT_EQ(fake.calls.size(), 3u);
  EXPECT_TRUE(processor.process(99, 101, 1280, 360, {0, 90, 640, 180}, 98));
  EXPECT_TRUE(processor.process(99, 101, 137, 79, {8, 0, 624, 360}, 98));
  ASSERT_TRUE(processor.initialize(display, 1280, 720, targets));
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "destroy-context"), 1);
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "context"), 2);
}

TEST(VaapiVideoProcessorTest, FullCanvasDoesNotAllocateOrSubmitAnExtraClear) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  ASSERT_TRUE(processor.process(99, 101, 640, 360, {0, 0, 640, 360}));
  ASSERT_EQ(fake.submissions.size(), 1u);
  EXPECT_EQ(fake.params.surface, 99u);
  EXPECT_EQ(fake.params.input_color_properties.color_range, VA_SOURCE_RANGE_FULL);
}

TEST(VaapiVideoProcessorTest, RejectsMissingOrAliasedBlackSurfaceBeforeSubmitting) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  for (auto black : {VASurfaceID(VA_INVALID_SURFACE), VASurfaceID(99), VASurfaceID(101)})
    EXPECT_FALSE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, black));
  EXPECT_TRUE(fake.submissions.empty());
  EXPECT_EQ(fake.calls.size(), 3u);
}

TEST(VaapiVideoProcessorTest, EveryPaddedDrawClearsTheWholeCanvasBeforeRgb) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  ASSERT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
  ASSERT_TRUE(processor.process(99, 101, 1280, 360, {0, 90, 640, 180}, 98));
  ASSERT_TRUE(processor.process(99, 101, 137, 79, {8, 0, 624, 360}, 98));
  ASSERT_EQ(fake.submissions.size(), 6u);
  for (size_t i = 0; i < fake.submissions.size(); i += 2) {
    const auto &clear = fake.submissions[i];
    EXPECT_EQ(clear.params.surface, 98u);
    EXPECT_EQ(clear.fit.x, 0); EXPECT_EQ(clear.fit.y, 0);
    EXPECT_EQ(clear.fit.width, 640); EXPECT_EQ(clear.fit.height, 360);
    EXPECT_EQ(fake.submissions[i + 1].params.surface, 99u);
  }
  // Each pass synchronizes and releases its parameter buffer before the next.
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "sync-output"), 6);
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "destroy-buffer"), 6);
}

class VaapiClearFailureTest : public ::testing::TestWithParam<const char *> {};
TEST_P(VaapiClearFailureTest, FailedClearCannotSubmitRgbAndCanRetry) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  fake.failAt = GetParam(); fake.failSource = 98;
  EXPECT_FALSE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
  ASSERT_EQ(fake.submissions.size(), 1u);
  EXPECT_EQ(fake.params.surface, 98u);
  EXPECT_FALSE(processor.error().isEmpty());
  fake.failAt.clear();
  ASSERT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
  EXPECT_EQ(fake.params.surface, 99u);
}
INSTANTIATE_TEST_SUITE_P(ClearFailures, VaapiClearFailureTest,
    ::testing::Values("buffer", "begin", "render", "end", "sync-output", "destroy-buffer"));

class VaapiRgbAfterClearFailureTest : public ::testing::TestWithParam<const char *> {};
TEST_P(VaapiRgbAfterClearFailureTest, FailedDrawAfterClearDoesNotReportSuccess) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  fake.failAt = GetParam(); fake.failSource = 99;
  EXPECT_FALSE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
  ASSERT_EQ(fake.submissions.size(), 2u);
  EXPECT_EQ(fake.submissions.front().params.surface, 98u);
  EXPECT_EQ(fake.params.surface, 99u);
  EXPECT_FALSE(processor.error().isEmpty());
  fake.failAt.clear();
  ASSERT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
}
INSTANTIATE_TEST_SUITE_P(RgbFailures, VaapiRgbAfterClearFailureTest,
    ::testing::Values("buffer", "begin", "render", "end", "sync-output", "destroy-buffer"));

TEST(VaapiVideoProcessorTest, AcceptsDriverOwnedColorCapabilitiesWithoutRetainingThem) {
  FakeVa fake;
  auto ops = FakeVa::ops();
  ops.queryCaps = [](VADisplay, VAContextID, VABufferID *, unsigned, VAProcPipelineCaps *caps) {
    // Mesa returns its own array rather than filling the supplied storage.
    auto &colors = FakeVa::current->driverColors;
    colors = {VAProcColorStandardBT709, VAProcColorStandardBT601};
    caps->output_color_standards = colors.data();
    caps->num_output_color_standards = colors.size();
    return FakeVa::call("caps");
  };
  VaapiVideoProcessor processor(ops);
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  EXPECT_TRUE(processor.error().isEmpty());
  fake.driverColors.clear();
  fake.driverColors.shrink_to_fit();
  ASSERT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
  EXPECT_EQ(fake.params.output_color_standard, VAProcColorStandardBT601);
  EXPECT_EQ(fake.params.output_background_color, 0xff000000u);
  EXPECT_EQ(fake.params.input_color_properties.color_range, VA_SOURCE_RANGE_FULL);
  EXPECT_EQ(fake.params.output_color_properties.color_range, VA_SOURCE_RANGE_REDUCED);
}

TEST(VaapiVideoProcessorTest, RejectsDriverOwnedListWithoutBt601DespiteSuppliedStorage) {
  FakeVa fake;
  auto ops = FakeVa::ops();
  ops.queryCaps = [](VADisplay, VAContextID, VABufferID *, unsigned, VAProcPipelineCaps *caps) {
    // Supplied storage is not authoritative after the driver replaces the pointer.
    caps->output_color_standards[0] = VAProcColorStandardBT601;
    static VAProcColorStandardType colors[]{VAProcColorStandardBT709};
    caps->output_color_standards = colors;
    caps->num_output_color_standards = 1;
    return FakeVa::call("caps");
  };
  VaapiVideoProcessor processor(ops);
  EXPECT_FALSE(processor.initialize(display, 640, 360, targets));
  EXPECT_TRUE(processor.error().contains("BT.601"));
  EXPECT_EQ(fake.calls, (std::vector<std::string>{"config", "context", "caps",
                                               "destroy-context", "destroy-config"}));
}

class VaapiColorListTest : public ::testing::TestWithParam<bool> {};
TEST_P(VaapiColorListTest, RejectsNullOrEmptyReturnedListAndCanRetry) {
  FakeVa fake;
  auto ops = FakeVa::ops();
  ops.queryCaps = [](VADisplay, VAContextID, VABufferID *, unsigned, VAProcPipelineCaps *caps) {
    caps->output_color_standards[0] = VAProcColorStandardBT601;
    if (FakeVa::current->bt601) {
      if (GetParam()) {
        caps->output_color_standards = nullptr;
        caps->num_output_color_standards = 1;
      } else {
        caps->num_output_color_standards = 0;
      }
    } else {
      caps->num_output_color_standards = 1;
    }
    return FakeVa::call("caps");
  };
  VaapiVideoProcessor processor(ops);
  EXPECT_FALSE(processor.initialize(display, 640, 360, targets));
  EXPECT_EQ(fake.calls.back(), "destroy-config");
  fake.bt601 = false; // return valid supplied storage on retry
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  EXPECT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
}
INSTANTIATE_TEST_SUITE_P(ReturnedCapabilities, VaapiColorListTest, ::testing::Bool());

TEST(VaapiVideoProcessorTest, RejectsUnsupportedColorOrGeometryBeforeSubmittingWork) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  fake.bt601 = false;
  EXPECT_FALSE(processor.initialize(display, 640, 360, targets));
  EXPECT_EQ(fake.calls.back(), "destroy-config");
  fake.bt601 = true; fake.maxInput = 2048;
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  const auto calls = fake.calls.size();
  EXPECT_FALSE(processor.process(99, 101, 4096, 2160, {0, 0, 640, 360}));
  EXPECT_FALSE(processor.process(99, 101, 320, 240, {-2, 0, 480, 360}));
  EXPECT_FALSE(processor.process(99, 101, 320, 240, {80, 0, 600, 360}));
  EXPECT_FALSE(processor.process(99, 101, 320, 240, {81, 0, 478, 360}));
  EXPECT_FALSE(processor.process(99, 999, 320, 240, {80, 0, 480, 360}));
  EXPECT_EQ(fake.calls.size(), calls);
}

class VaapiVideoProcessorFailureTest : public ::testing::TestWithParam<const char *> {};
TEST_P(VaapiVideoProcessorFailureTest, UnwindsAndCanRetry) {
  FakeVa fake;
  fake.failAt = GetParam();
  VaapiVideoProcessor processor(FakeVa::ops());
  const bool initialized = processor.initialize(display, 640, 360, targets);
  if (fake.failAt == "config" || fake.failAt == "context" || fake.failAt == "caps")
    EXPECT_FALSE(initialized);
  else
    ASSERT_TRUE(initialized);
  if (initialized) {
    EXPECT_FALSE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
    if (fake.failAt == "render" || fake.failAt == "end" || fake.failAt == "sync-output") {
      EXPECT_NE(std::find(fake.calls.begin(), fake.calls.end(), "end"), fake.calls.end());
      EXPECT_NE(std::find(fake.calls.begin(), fake.calls.end(), "sync-output"), fake.calls.end());
      EXPECT_NE(std::find(fake.calls.begin(), fake.calls.end(), "sync-source"), fake.calls.end());
      EXPECT_EQ(fake.calls.back(), "destroy-buffer");
    }
  }
  EXPECT_FALSE(processor.error().isEmpty());
  fake.failAt.clear();
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  EXPECT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
}
INSTANTIATE_TEST_SUITE_P(NativeFailures, VaapiVideoProcessorFailureTest,
    ::testing::Values("config", "context", "caps", "buffer", "begin", "render", "end", "sync-output", "destroy-buffer"));

TEST(VaapiVideoProcessorTest, MissingEntryPointsAndEmptyPoolsFailWithoutNativeCalls) {
  FakeVa fake;
  VaapiVideoProcessor incomplete(VaapiVideoProcessor::Ops{});
  EXPECT_FALSE(incomplete.initialize(display, 640, 360, targets));
  VaapiVideoProcessor processor(FakeVa::ops());
  EXPECT_FALSE(processor.initialize(display, 640, 360, {}));
  EXPECT_FALSE(processor.initialize(nullptr, 640, 360, targets));
  EXPECT_TRUE(fake.calls.empty());
}

TEST(VaapiVideoProcessorTest, RepeatedDestroyFailureCannotAccumulateBuffersOrSubmitMoreWork) {
  FakeVa fake;
  VaapiVideoProcessor processor(FakeVa::ops());
  ASSERT_TRUE(processor.initialize(display, 640, 360, targets));
  fake.failAt = "destroy-buffer";
  EXPECT_FALSE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
  const auto firstCalls = fake.calls;
  for (int retry = 0; retry < 3; ++retry) {
    EXPECT_FALSE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
    EXPECT_EQ(fake.calls.back(), "destroy-buffer");
    EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "buffer"), 1);
    EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "begin"), 1);
    EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "sync-output"), 1);
  }
  EXPECT_EQ(fake.calls.size(), firstCalls.size() + 3);
  EXPECT_FALSE(processor.error().isEmpty());
  fake.failAt.clear();
  ASSERT_TRUE(processor.process(99, 101, 320, 240, {80, 0, 480, 360}, 98));
  EXPECT_TRUE(processor.error().isEmpty());
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "buffer"), 3);
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "begin"), 3);
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "context"), 1);
}
