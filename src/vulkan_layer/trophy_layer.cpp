#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <map>
#include <cstring>
#include <dlfcn.h>

#define LAYER_NAME "VK_LAYER_TROPHY_detection"
#define SOCKET_PATH "/tmp/trophy-detection.sock"

namespace {

struct LayerData {
    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr;
    int socket_fd = -1;
};

std::mutex g_lock;
std::map<void*, LayerData> g_instance_data;

// Helper to get the dispatch key from a dispatchable object
void* get_dispatch_key(void* obj) {
    return *(void**)obj;
}

void notify_app(int& fd, const char* app_name) {
    if (fd == -1) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == -1) return;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(fd);
            fd = -1;
            return;
        }
    }

    const char* steam_appid = getenv("SteamAppId");
    if (!steam_appid) steam_appid = "0";

    std::string msg = "START:" + std::to_string(getpid()) + ":" + 
                     (app_name ? app_name : "Unknown Vulkan Game") + ":" + 
                     steam_appid + "\n";
    send(fd, msg.c_str(), msg.length(), 0);
}

// Layer function implementations
VKAPI_ATTR VkResult VKAPI_CALL Trophy_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    
    VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;

    // Step through the chain of create info until we find the one for the loader
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                               layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerInstanceCreateInfo*)layerCreateInfo->pNext;
    }

    if (layerCreateInfo == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult result = createFunc(pCreateInfo, pAllocator, pInstance);

    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_lock);
        LayerData& data = g_instance_data[get_dispatch_key(*pInstance)];
        data.nextGetInstanceProcAddr = gipa;
        
        const char* app_name = nullptr;
        if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pApplicationName) {
            app_name = pCreateInfo->pApplicationInfo->pApplicationName;
        }
        
        notify_app(data.socket_fd, app_name);
    }

    return result;
}

VKAPI_ATTR void VKAPI_CALL Trophy_DestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {
    
    std::unique_lock<std::mutex> lock(g_lock);
    auto it = g_instance_data.find(get_dispatch_key(instance));
    if (it != g_instance_data.end()) {
        PFN_vkDestroyInstance destroyFunc = (PFN_vkDestroyInstance)it->second.nextGetInstanceProcAddr(instance, "vkDestroyInstance");
        
        if (it->second.socket_fd != -1) {
            std::string msg = "STOP:" + std::to_string(getpid()) + "\n";
            send(it->second.socket_fd, msg.c_str(), msg.length(), 0);
            close(it->second.socket_fd);
        }
        
        PFN_vkGetInstanceProcAddr nextGIPA = it->second.nextGetInstanceProcAddr;
        g_instance_data.erase(it);
        lock.unlock(); // Unlock before calling down the chain

        destroyFunc(instance, pAllocator);
    }
}

} // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Trophy_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (strcmp(pName, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)Trophy_CreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)Trophy_DestroyInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)Trophy_GetInstanceProcAddr;

    if (instance == VK_NULL_HANDLE) return nullptr;

    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_instance_data.find(get_dispatch_key(instance));
    if (it != g_instance_data.end()) {
        return it->second.nextGetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

// Entry point for the loader to negotiate interface version
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;

    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr = Trophy_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = nullptr; // We are an instance-only layer
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }

    if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    }

    return VK_SUCCESS;
}

} // extern "C"
