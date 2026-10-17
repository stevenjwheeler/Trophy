/* Real layer entry points; no GPU, real VkInstance, or external socket needed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "VulkanReporterFixture.h"
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
// Reuse one isolated namespace: glibc/libvulkan can retain static TLS even
// after dlclose, so a namespace per test exhausts TLS during --gtest_repeat.
// Every successful instance is destroyed by TearDown; only the idle watcher
// survives between tests and is joined when this owner unloads the layer.
struct IsolatedFixtureOwner {
  void *handle = dlmopen(LM_ID_NEWLM, VULKAN_REPORTER_FIXTURE_FILE,
                        RTLD_NOW | RTLD_LOCAL);
  ~IsolatedFixtureOwner() {
    if (handle)
      dlclose(handle);
  }
};

void *isolatedFixture() {
  static IsolatedFixtureOwner owner;
  return owner.handle;
}
}

TEST(VulkanReporterTest, DestroyNullInstanceIsNoop) {
  void *layer = dlopen(TROPHY_LAYER_FILE, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(layer, nullptr) << dlerror();
  // The layer does not export vkDestroyInstance. dlsym of that name silently
  // returns its libvulkan dependency, testing the loader rather than Trophy.
  auto negotiate = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
      dlsym(layer, "vkNegotiateLoaderLayerInterfaceVersion"));
  ASSERT_NE(negotiate, nullptr);
  VkNegotiateLayerInterface version{};
  version.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
  version.loaderLayerInterfaceVersion = 2;
  ASSERT_EQ(negotiate(&version), VK_SUCCESS);
  ASSERT_NE(version.pfnGetInstanceProcAddr, nullptr);
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
      version.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroyInstance"));
  ASSERT_NE(destroy, nullptr);
  destroy(VK_NULL_HANDLE, nullptr);
  dlclose(layer);
}

class IsolatedVulkanReporterTest : public ::testing::Test {
protected:
  void SetUp() override {
    fixture = isolatedFixture();
    ASSERT_NE(fixture, nullptr) << dlerror();
    auto entry = reinterpret_cast<decltype(&VulkanReporterLayerEntry)>(
        dlsym(fixture, "VulkanReporterLayerEntry"));
    reset = reinterpret_cast<decltype(&VulkanReporterReset)>(
        dlsym(fixture, "VulkanReporterReset"));
    read = reinterpret_cast<decltype(&VulkanReporterRead)>(
        dlsym(fixture, "VulkanReporterRead"));
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(reset, nullptr);
    ASSERT_NE(read, nullptr);
    gipa = entry();
    ASSERT_NE(gipa, nullptr);
    destroy = reinterpret_cast<PFN_vkDestroyInstance>(
        gipa(VK_NULL_HANDLE, "vkDestroyInstance"));
    create = reinterpret_cast<PFN_vkCreateInstance>(
        gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    ASSERT_NE(destroy, nullptr);
    ASSERT_NE(create, nullptr);
    reset(VK_SUCCESS);
  }

  void TearDown() override {
    if (liveInstance && destroy)
      destroy(liveInstance, nullptr);
  }

  VulkanReporterSnapshot snapshot() {
    VulkanReporterSnapshot value{};
    read(&value);
    return value;
  }

  VkResult createInstance(const char *name) {
    auto nextGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(fixture, "vkGetInstanceProcAddr"));
    VkLayerInstanceLink link{};
    link.pfnNextGetInstanceProcAddr = nextGipa;
    VkLayerInstanceCreateInfo layerInfo{};
    layerInfo.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
    layerInfo.function = VK_LAYER_LINK_INFO;
    layerInfo.u.pLayerInfo = &link;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = name;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pNext = &layerInfo;
    info.pApplicationInfo = &app;
    return create(&info, nullptr, &liveInstance);
  }

  void *fixture = nullptr;
  decltype(&VulkanReporterReset) reset = nullptr;
  decltype(&VulkanReporterRead) read = nullptr;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkDestroyInstance destroy = nullptr;
  PFN_vkCreateInstance create = nullptr;
  VkInstance liveInstance = VK_NULL_HANDLE;
};

TEST_F(IsolatedVulkanReporterTest, UnknownInstanceForwardsExactlyOnceWithAllocator) {
  int dispatchToken;
  struct { void *dispatch; } unknown{&dispatchToken};
  const auto handle = reinterpret_cast<VkInstance>(&unknown);
  VkAllocationCallbacks allocator{};
  destroy(handle, &allocator);
  const auto result = snapshot();
  EXPECT_EQ(result.destroyCalls, 1u);
  EXPECT_EQ(result.destroyedInstance, handle);
  EXPECT_EQ(result.destroyedAllocator, &allocator);
  EXPECT_EQ(result.socketCalls, 0u);
  EXPECT_STREQ(result.messages, "");
}

TEST_F(IsolatedVulkanReporterTest, NullInstanceDoesNotForwardOrReport) {
  destroy(VK_NULL_HANDLE, nullptr);
  const auto result = snapshot();
  EXPECT_EQ(result.destroyCalls, 0u);
  EXPECT_EQ(result.socketCalls, 0u);
  EXPECT_STREQ(result.messages, "");
}

TEST_F(IsolatedVulkanReporterTest, FailedCreateDoesNotTrackOrReport) {
  reset(VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(createInstance("Failed game"), VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(liveInstance, VK_NULL_HANDLE);
  const auto result = snapshot();
  EXPECT_EQ(result.createCalls, 1u);
  EXPECT_EQ(result.destroyCalls, 0u);
  EXPECT_EQ(result.socketCalls, 0u);
  EXPECT_STREQ(result.messages, "");
}

TEST_F(IsolatedVulkanReporterTest, SharedDispatchTableDoesNotMergeInstanceOwnership) {
  ASSERT_EQ(createInstance("First"), VK_SUCCESS);
  const VkInstance first = liveInstance;
  liveInstance = VK_NULL_HANDLE;
  ASSERT_EQ(createInstance("Second"), VK_SUCCESS);
  const VkInstance second = liveInstance;
  EXPECT_NE(first, second);
  destroy(first, nullptr);
  EXPECT_EQ(snapshot().closeCalls, 1u);
  destroy(second, nullptr);
  liveInstance = VK_NULL_HANDLE;
  const auto result = snapshot();
  EXPECT_EQ(result.destroyCalls, 2u);
  EXPECT_EQ(result.closeCalls, 2u);
  EXPECT_EQ(result.sendCalls, 4u);
}

TEST_F(IsolatedVulkanReporterTest, InvalidCreateAndLookupInputsAreRejected) {
  VkInstance instance = VK_NULL_HANDLE;
  EXPECT_EQ(create(nullptr, nullptr, &instance), VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(gipa(VK_NULL_HANDLE, nullptr), nullptr);
  EXPECT_EQ(snapshot().createCalls, 0u);
}

TEST_F(IsolatedVulkanReporterTest, ApplicationNameCannotInjectProtocolLines) {
  ASSERT_EQ(createInstance("Game\nSTOP:123\r"), VK_SUCCESS);
  const std::string messages = snapshot().messages;
  EXPECT_EQ(messages.find(":Game STOP:123 :"), messages.find("START:") +
      std::string("START:").size() + std::to_string(getpid()).size());
  EXPECT_EQ(messages.find('\n'), messages.size() - 1);
}

TEST_F(IsolatedVulkanReporterTest, TrackedInstanceReportsStartAndStopAndForwards) {
  ASSERT_EQ(createInstance("Game: With Colon"), VK_SUCCESS);
  ASSERT_NE(liveInstance, VK_NULL_HANDLE);
  const auto handle = liveInstance;
  VkAllocationCallbacks allocator{};
  destroy(handle, &allocator);
  liveInstance = VK_NULL_HANDLE;
  const auto result = snapshot();
  EXPECT_EQ(result.createCalls, 1u);
  EXPECT_EQ(result.destroyCalls, 1u);
  EXPECT_EQ(result.destroyedInstance, handle);
  EXPECT_EQ(result.destroyedAllocator, &allocator);
  EXPECT_EQ(result.socketCalls, 1u);
  EXPECT_EQ(result.connectCalls, 1u);
  EXPECT_EQ(result.closeCalls, 1u);
  EXPECT_EQ(result.sendCalls, 2u);
  const char *steamId = getenv("SteamAppId");
  const std::string pid = std::to_string(getpid());
  EXPECT_EQ(std::string(result.messages),
            "START:" + pid + ":Game: With Colon:" +
                (steamId ? steamId : "0") + "\nSTOP:" + pid + "\n");
  EXPECT_EQ(gipa(handle, "vkNonexistentFunction"), nullptr);
}

TEST_F(IsolatedVulkanReporterTest, FailedStartSendReconnectsAndResends) {
  auto failNextSend = reinterpret_cast<decltype(&VulkanReporterFailNextSend)>(
      dlsym(fixture, "VulkanReporterFailNextSend"));
  ASSERT_NE(failNextSend, nullptr);
  failNextSend();
  ASSERT_EQ(createInstance("Reconnect game"), VK_SUCCESS);
  ASSERT_NE(liveInstance, VK_NULL_HANDLE);
  // EPIPE must drop the cached connection; otherwise the watcher sees it as
  // healthy (our recv returns EAGAIN) and never retries the lost START.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  // Wait for the next health check as well as send: notify_app sends before
  // the watcher publishes its replacement fd under the layer's mutex.
  while (snapshot().recvCalls == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto reconnected = snapshot();
  ASSERT_GT(reconnected.recvCalls, 0u) << "watcher did not publish the new fd";
  ASSERT_EQ(reconnected.sendCalls, 2u) << "watcher never resent the failed START";
  EXPECT_EQ(reconnected.socketCalls, 2u);
  EXPECT_EQ(reconnected.connectCalls, 2u);
  EXPECT_EQ(reconnected.closeCalls, 1u);
  const char *steamId = getenv("SteamAppId");
  const std::string pid = std::to_string(getpid());
  const std::string start = "START:" + pid + ":Reconnect game:" +
                            (steamId ? steamId : "0") + "\n";
  EXPECT_EQ(std::string(reconnected.messages), start);

  destroy(liveInstance, nullptr);
  liveInstance = VK_NULL_HANDLE;
  const auto stopped = snapshot();
  EXPECT_EQ(stopped.destroyCalls, 1u);
  EXPECT_EQ(stopped.closeCalls, 2u);
  EXPECT_EQ(stopped.sendCalls, 3u);
  EXPECT_EQ(std::string(stopped.messages), start + "STOP:" + pid + "\n");
}
