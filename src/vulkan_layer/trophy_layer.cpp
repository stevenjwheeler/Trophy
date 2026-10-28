#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <map>
#include <cstring>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <algorithm>

// Independent integer helper revision, not the app/socket protocol version.
extern "C" {
__attribute__((used, visibility("default")))
extern const char trophy_vulkan_helper_version[] =
    "TROPHY_HELPER_VERSION:vulkan:1";
}

#define LAYER_NAME "VK_LAYER_TROPHY_detection"
#define SOCKET_PATH "/tmp/trophy-detection.sock"

namespace {

struct LayerData {
    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr;
    int socket_fd = -1;
    std::string app_name;
    uint64_t generation = 0;
};

std::mutex g_lock;
std::map<void*, LayerData> g_instance_data;
uint64_t g_next_generation = 0; // guarded by g_lock
std::atomic_bool g_watch_stop{false};
std::once_flag g_watch_once;
std::thread g_connection_watch;

bool socket_is_alive(int fd) {
    char byte;
    const ssize_t result = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return result > 0 ||
           (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                           errno == EINTR));
}

void notify_app(int& fd, const char* app_name) {
    // Revalidate a cached connection so a Trophy restart causes the next
    // instance creation to reconnect and report again.
    if (fd != -1) {
        if (!socket_is_alive(fd)) {
            close(fd);
            fd = -1;
        }
    }
    if (fd == -1) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
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
    if (!steam_appid || !*steam_appid || strnlen(steam_appid, 11) > 10 ||
        strspn(steam_appid, "0123456789") != strlen(steam_appid) ||
        strtoull(steam_appid, nullptr, 10) > UINT32_MAX)
        steam_appid = "0";

    std::string name(app_name ? app_name : "Unknown Vulkan Game",
                     strnlen(app_name ? app_name : "Unknown Vulkan Game", 256));
    std::replace(name.begin(), name.end(), '\n', ' ');
    std::replace(name.begin(), name.end(), '\r', ' ');

    std::string msg = "START:" + std::to_string(getpid()) + ":" + 
                     name + ":" +
                     steam_appid + "\n";
    if (send(fd, msg.c_str(), msg.length(), MSG_NOSIGNAL | MSG_DONTWAIT) !=
        static_cast<ssize_t>(msg.length())) {
        // Server went away mid-report: drop the cached fd so the
        // watcher reconnects and resends instead of trusting a dead fd.
        close(fd);
        fd = -1;
    }
}

void ensureConnectionWatch() {
    std::call_once(g_watch_once, [] {
        g_connection_watch = std::thread([] {
            while (!g_watch_stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                // Snapshot reconnect candidates under lock. Networking is
                // nonblocking and runs unlocked; backlog pressure is retried
                // on the next tick, never waited out on a loader thread.
                struct Reconnect { void *instance; std::string name; uint64_t generation; };
                std::vector<Reconnect> pending;
                {
                    std::lock_guard<std::mutex> lock(g_lock);
                    for (auto& entry : g_instance_data) {
                        if (entry.second.socket_fd == -1) {
                            pending.push_back({entry.first, entry.second.app_name,
                                               entry.second.generation});
                        } else if (!socket_is_alive(entry.second.socket_fd)) {
                            // Tear the dead fd down here so no open
                            // descriptor crosses the unlock boundary.
                            close(entry.second.socket_fd);
                            entry.second.socket_fd = -1;
                            pending.push_back({entry.first, entry.second.app_name,
                                               entry.second.generation});
                        }
                    }
                }
                for (auto& item : pending) {
                    int fresh = -1;
                    notify_app(fresh, item.name.c_str());
                    std::lock_guard<std::mutex> lock(g_lock);
                    auto it = g_instance_data.find(item.instance);
                    if (it == g_instance_data.end() ||
                        it->second.generation != item.generation) {
                        // Instance destroyed while we connected: drop ours.
                        if (fresh != -1)
                            close(fresh);
                    } else if (it->second.socket_fd == -1) {
                        it->second.socket_fd = fresh;
                    } else if (fresh != -1) {
                        // Another path reconnected first; drop our copy.
                        close(fresh);
                    }
                }
            }
        });
    });
}

__attribute__((destructor)) void stopConnectionWatch() {
    g_watch_stop.store(true, std::memory_order_release);
    if (g_connection_watch.joinable())
        g_connection_watch.join();
}

// Layer function implementations
VKAPI_ATTR VkResult VKAPI_CALL Trophy_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    
    if (!pCreateInfo || !pInstance)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;

    // Step through the chain of create info until we find the one for the loader
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                               layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerInstanceCreateInfo*)layerCreateInfo->pNext;
    }

    if (!layerCreateInfo || !layerCreateInfo->u.pLayerInfo ||
        !layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!createFunc)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = createFunc(pCreateInfo, pAllocator, pInstance);

    if (result == VK_SUCCESS) {
        // Loader dispatch tables may be shared by several instances. The
        // instance handle, not its first word, uniquely owns this state.
        void* key = *pInstance;
        const char* app_name = nullptr;
        if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pApplicationName) {
            app_name = pCreateInfo->pApplicationInfo->pApplicationName;
        }
        const std::string name = app_name ? app_name : "Unknown Vulkan Game";
        {
            // Map mutation stays under lock. A fresh entry starts
            // disconnected (fd -1) so no open fd crosses below.
            std::lock_guard<std::mutex> lock(g_lock);
            LayerData& data = g_instance_data[key];
            data.nextGetInstanceProcAddr = gipa;
            data.app_name = name;
            data.generation = ++g_next_generation;
        }
        // Nonblocking notification runs without the loader-state lock.
        int fresh = -1;
        notify_app(fresh, name.c_str());
        {
            std::lock_guard<std::mutex> lock(g_lock);
            auto it = g_instance_data.find(key);
            if (it == g_instance_data.end()) {
                if (fresh != -1)
                    close(fresh);
            } else if (it->second.socket_fd == -1) {
                it->second.socket_fd = fresh;
            } else if (fresh != -1) {
                // Watcher reconnected first; drop our copy.
                close(fresh);
            }
        }
        ensureConnectionWatch();
    }

    return result;
}

VKAPI_ATTR void VKAPI_CALL Trophy_DestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {
    // vkDestroyInstance(VK_NULL_HANDLE) is a legal no-op: never
    // dereference the handle to look up dispatch state.
    if (instance == VK_NULL_HANDLE)
        return;

    std::unique_lock<std::mutex> lock(g_lock);
    auto it = g_instance_data.find(instance);
    if (it != g_instance_data.end()) {
        // Copy everything needed, then drop the lock before any
        // down-call: resolving the destroy pointer invokes the driver.
        PFN_vkGetInstanceProcAddr nextGIPA = it->second.nextGetInstanceProcAddr;
        const int fd = it->second.socket_fd;
        g_instance_data.erase(it);
        lock.unlock(); // Unlock before calling down the chain

        PFN_vkDestroyInstance destroyFunc = (PFN_vkDestroyInstance)nextGIPA(instance, "vkDestroyInstance");
        if (fd != -1) {
            std::string msg = "STOP:" + std::to_string(getpid()) + "\n";
            send(fd, msg.c_str(), msg.length(),
                  MSG_NOSIGNAL | MSG_DONTWAIT);
            close(fd);
        }

        destroyFunc(instance, pAllocator);
        return;
    }
    lock.unlock();

    // Unknown instance (e.g. created before the layer attached): still
    // forward to the real destroy so the VkInstance never leaks.
    if (PFN_vkDestroyInstance fallback =
            (PFN_vkDestroyInstance)dlsym(RTLD_NEXT, "vkDestroyInstance")) {
        fallback(instance, pAllocator);
    }
}

} // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Trophy_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (strcmp(pName, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)Trophy_CreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)Trophy_DestroyInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)Trophy_GetInstanceProcAddr;

    if (instance == VK_NULL_HANDLE) return nullptr;

    PFN_vkGetInstanceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_instance_data.find(instance);
        if (it != g_instance_data.end()) {
            next = it->second.nextGetInstanceProcAddr;
        }
    }
    // Down-call runs without g_lock; only the copied function pointer is
    // used, so a concurrent DestroyInstance erase can't strand us.
    if (next) {
        return next(instance, pName);
    }
    return nullptr;
}

// Entry point for the loader to negotiate interface version
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        pVersionStruct->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

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
