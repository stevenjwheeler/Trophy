#include "VulkanReporterFixture.h"
#include <vulkan/vk_layer.h>

extern "C" VkResult vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *version);

// Link this DSO against trophy_layer FIRST and VulkanReporterNext SECOND.
// Both must be direct DT_NEEDED entries: RTLD_NEXT in the real layer then
// encounters our fake before the layer's libvulkan dependency. dlmopen keeps
// these symbols out of the test runner and the user's application namespace.
extern "C" PFN_vkGetInstanceProcAddr VulkanReporterLayerEntry() {
    VulkanReporterFakeAnchor();
    VkNegotiateLayerInterface version{};
    version.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    version.loaderLayerInterfaceVersion = 2;
    if (vkNegotiateLoaderLayerInterfaceVersion(&version) != VK_SUCCESS)
        return nullptr;
    return version.pfnGetInstanceProcAddr;
}
