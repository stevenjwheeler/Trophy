#include "VulkanReporterFixture.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>

namespace {
std::mutex mutex;
VulkanReporterSnapshot state{};
VkResult createResult = VK_SUCCESS;
bool failNextSend = false;
int dispatchToken;
struct FakeInstance { void *dispatch; } instances[16];
unsigned nextInstance = 0;
constexpr int fakeFd = 123456;

VkResult VKAPI_CALL create(const VkInstanceCreateInfo *,
                          const VkAllocationCallbacks *, VkInstance *output) {
    std::lock_guard<std::mutex> lock(mutex);
    ++state.createCalls;
    if (createResult == VK_SUCCESS) {
        auto &instance = instances[nextInstance++ % 16];
        instance.dispatch = &dispatchToken;
        *output = reinterpret_cast<VkInstance>(&instance);
    }
    return createResult;
}
}

extern "C" void VulkanReporterFakeAnchor() {}

extern "C" void VulkanReporterReset(VkResult result) {
    std::lock_guard<std::mutex> lock(mutex);
    state = {};
    createResult = result;
    failNextSend = false;
}

extern "C" void VulkanReporterRead(VulkanReporterSnapshot *snapshot) {
    std::lock_guard<std::mutex> lock(mutex);
    *snapshot = state;
}

extern "C" void VulkanReporterFailNextSend() {
    std::lock_guard<std::mutex> lock(mutex);
    failNextSend = true;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance handle, const VkAllocationCallbacks *allocator) {
    std::lock_guard<std::mutex> lock(mutex);
    ++state.destroyCalls;
    state.destroyedInstance = handle;
    state.destroyedAllocator = allocator;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance, const char *name) {
    if (std::strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(create);
    if (std::strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
    return nullptr;
}

// Never forward any networking call. These overrides are local to dlmopen's
// namespace and make successful reporter creation safe even with live Trophy.
extern "C" int socket(int domain, int type, int) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    ++state.socketCalls;
    if (domain != AF_UNIX || type != (SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return fakeFd;
}

extern "C" int connect(int fd, const sockaddr *address, socklen_t size) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    ++state.connectCalls;
    if (fd != fakeFd || size < sizeof(sockaddr_un) ||
        address->sa_family != AF_UNIX ||
        std::strcmp(reinterpret_cast<const sockaddr_un *>(address)->sun_path,
                    "/tmp/trophy-detection.sock") != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

extern "C" ssize_t send(int fd, const void *bytes, size_t size, int flags) {
    std::lock_guard<std::mutex> lock(mutex);
    ++state.sendCalls;
    if (failNextSend) {
        failNextSend = false;
        errno = EPIPE;
        return -1;
    }
    const size_t used = std::strlen(state.messages);
    if (fd != fakeFd || flags != (MSG_NOSIGNAL | MSG_DONTWAIT) ||
        size >= sizeof(state.messages) - used) {
        errno = EINVAL;
        return -1;
    }
    std::memcpy(state.messages + used, bytes, size);
    state.messages[used + size] = '\0';
    return static_cast<ssize_t>(size);
}

extern "C" ssize_t recv(int, void *, size_t, int) {
    std::lock_guard<std::mutex> lock(mutex);
    ++state.recvCalls;
    errno = EAGAIN;
    return -1;
}

extern "C" int close(int fd) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd != fakeFd) {
        errno = EBADF;
        return -1;
    }
    ++state.closeCalls;
    return 0;
}
