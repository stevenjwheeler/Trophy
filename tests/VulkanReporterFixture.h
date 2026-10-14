#pragma once

#include <vulkan/vulkan.h>

// POD-only interface across the isolated dynamic-loader namespace.
struct VulkanReporterSnapshot {
    unsigned createCalls;
    unsigned destroyCalls;
    unsigned socketCalls;
    unsigned connectCalls;
    unsigned sendCalls;
    unsigned recvCalls;
    unsigned closeCalls;
    VkInstance destroyedInstance;
    const VkAllocationCallbacks *destroyedAllocator;
    char messages[2048];
};

extern "C" {
void VulkanReporterReset(VkResult createResult);
void VulkanReporterRead(VulkanReporterSnapshot *snapshot);
void VulkanReporterFailNextSend();
PFN_vkGetInstanceProcAddr VulkanReporterLayerEntry();
void VulkanReporterFakeAnchor();
}
