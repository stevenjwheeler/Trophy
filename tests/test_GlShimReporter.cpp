/* Compile the actual shim with private native-operation substitutes. No
 * preload, graphics driver, detection socket or anti-cheat is contacted. */
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <string>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {
int sockets, closes, sends, socketType, sendFlags, connectResult;
int swapCalls, swapErrno;
bool shortSend, dead;
pid_t fakePid = 12345;
long fakeTime = 100;
std::string message;
std::atomic<bool> holdSend{false}, enteredSend{false}, releaseSend{false};
int fakeSocket(int, int type, int) { ++sockets; socketType = type; return 99; }
int fakeConnect(int, const sockaddr *, socklen_t) { errno = EAGAIN; return connectResult; }
int fakeClose(int fd) {
    if (fd != 99) return ::close(fd); // Real /proc/self/comm fallback read.
    ++closes; return 0;
}
ssize_t fakeSend(int, const void *bytes, size_t count, int flags) {
    ++sends;
    sendFlags = flags;
    if (holdSend.load()) {
        enteredSend = true;
        while (!releaseSend.load()) std::this_thread::yield();
    }
    message.assign(static_cast<const char *>(bytes), count);
    errno = EPIPE;
    return shortSend ? static_cast<ssize_t>(count - 1) : static_cast<ssize_t>(count);
}
ssize_t fakeRecv(int, void *, size_t, int) { errno = EAGAIN; return dead ? 0 : -1; }
int fakeClock(clockid_t, timespec *ts) { ts->tv_sec = fakeTime; return 0; }
pid_t fakeGetpid() { return fakePid; }
void *fakeDlsym(void *, const char *) { errno = ENOENT; return nullptr; }
int realEgl(void *, void *) { ++swapCalls; swapErrno = errno; return 17; }
void realGlx(void *, unsigned long) { ++swapCalls; swapErrno = errno; }
void realSdl(void *) { ++swapCalls; swapErrno = errno; }
}

#define socket fakeSocket
#define connect fakeConnect
#define close fakeClose
#define send fakeSend
#define recv fakeRecv
#define clock_gettime fakeClock
#define getpid fakeGetpid
#define dlsym fakeDlsym
#include "../src/gl_shim/trophy_gl_shim.c"
#undef socket
#undef connect
#undef close
#undef send
#undef recv
#undef clock_gettime
#undef getpid
#undef dlsym

class GlShimReporterTest : public ::testing::Test {
protected:
    std::string oldSteam, oldDisable;
    bool hadSteam, hadDisable;
    void SetUp() override {
        const char *value = getenv("SteamAppId");
        hadSteam = value; oldSteam = value ? value : "";
        value = getenv("DISABLE_TROPHY_DETECTION");
        hadDisable = value; oldDisable = value ? value : "";
        unsetenv("DISABLE_TROPHY_DETECTION");
        setenv("SteamAppId", "42", 1);
        sockets = closes = sends = swapCalls = swapErrno = 0;
        connectResult = 0; shortSend = dead = false;
        holdSend = enteredSend = releaseSend = false;
        fakePid = 12345; fakeTime = 100;
        g_sock = -1; g_notified = 0; g_disabled = -1;
        g_report_pid = fakePid; g_notify_owner = 0;
        g_last_health_check = g_last_connect_attempt = 0;
        g_real_egl_swap = realEgl; g_real_glx_swap = realGlx;
        g_real_sdl_swap = realSdl;
        message.clear();
    }
    void TearDown() override {
        trophy_gl_unload();
        if (hadSteam) setenv("SteamAppId", oldSteam.c_str(), 1); else unsetenv("SteamAppId");
        if (hadDisable) setenv("DISABLE_TROPHY_DETECTION", oldDisable.c_str(), 1);
        else unsetenv("DISABLE_TROPHY_DETECTION");
    }
};

TEST_F(GlShimReporterTest, ForwardsAllHooksAndPreservesErrnoAndEglResult) {
    errno = EDOM;
    EXPECT_EQ(eglSwapBuffers(nullptr, nullptr), 17);
    EXPECT_EQ(swapErrno, EDOM);
    glXSwapBuffers(nullptr, 0);
    SDL_GL_SwapWindow(nullptr);
    EXPECT_EQ(swapCalls, 3);
    EXPECT_EQ(sockets, 1);
    EXPECT_EQ(socketType, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK);
    EXPECT_EQ(sendFlags, MSG_NOSIGNAL | MSG_DONTWAIT);
    EXPECT_EQ(message, "START:12345\n");
}

TEST_F(GlShimReporterTest, DisabledStillForwardsWithoutConnecting) {
    setenv("DISABLE_TROPHY_DETECTION", "1", 1);
    EXPECT_EQ(eglSwapBuffers(nullptr, nullptr), 17);
    ++fakeTime;
    glXSwapBuffers(nullptr, 0);
    EXPECT_EQ(swapCalls, 2);
    EXPECT_EQ(sockets, 0);
}

TEST_F(GlShimReporterTest, MissingDownstreamDoesNotReportOrFakeSuccess) {
    g_real_egl_swap = nullptr;
    errno = EDOM;
    EXPECT_EQ(eglSwapBuffers(nullptr, nullptr), 0);
    EXPECT_EQ(errno, EDOM);
    EXPECT_EQ(sockets, 0);
}

TEST_F(GlShimReporterTest, BacklogFailureIsRateLimitedAndRetried) {
    connectResult = -1;
    notify_start(); notify_start();
    EXPECT_EQ(sockets, 1);
    EXPECT_EQ(g_notified, 0);
    ++fakeTime; connectResult = 0;
    notify_start();
    EXPECT_EQ(sockets, 2);
    EXPECT_EQ(g_notified, 2);
}

TEST_F(GlShimReporterTest, ShortSendClosesAndRetriesInsteadOfClaimingLive) {
    shortSend = true;
    notify_start();
    EXPECT_EQ(g_notified, 0);
    EXPECT_EQ(g_sock, -1);
    EXPECT_EQ(closes, 1);
    shortSend = false; ++fakeTime;
    notify_start();
    EXPECT_EQ(g_notified, 2);
}

TEST_F(GlShimReporterTest, RestartReconnectsOnLaterSwap) {
    notify_start(); dead = true; ++fakeTime;
    notify_start();
    EXPECT_EQ(g_sock, -1);
    dead = false; ++fakeTime;
    notify_start();
    EXPECT_EQ(sockets, 2);
    EXPECT_EQ(g_notified, 2);
}

TEST_F(GlShimReporterTest, EnvironmentCannotInjectProtocolLines) {
    // The shim no longer reads SteamAppId at all: identity is resolved
    // server-side. Environment content must never change or extend the line.
    for (const char *id : {"42\nSTOP:12345", "4294967296", "-1", "", "123456789012"}) {
        setenv("SteamAppId", id, 1);
        g_notified = 0; ++fakeTime;
        notify_start();
        const std::string expected = "START:12345\n";
        ASSERT_GE(message.size(), expected.size());
        EXPECT_EQ(message.compare(message.size() - expected.size(),
                                  expected.size(), expected), 0);
        EXPECT_EQ(message.find("STOP:"), std::string::npos);
        trophy_gl_unload();
    }
}

TEST_F(GlShimReporterTest, ConcurrentSwapDoesNotWaitOrCorruptConnectingState) {
    holdSend = true;
    std::thread first([] { notify_start(); });
    while (!enteredSend.load()) std::this_thread::yield();
    // If the guard blocks, this test times out; it must simply skip reporting.
    EXPECT_EQ(eglSwapBuffers(nullptr, nullptr), 17);
    releaseSend = true;
    first.join();
    EXPECT_EQ(sockets, 1);
    EXPECT_EQ(g_notified, 2);
    EXPECT_EQ(g_notify_owner, 0);
}

TEST_F(GlShimReporterTest, ForkRecoversInheritedOwnerAndSocket) {
    notify_start();
    g_notify_owner = fakePid;
    ++fakePid; ++fakeTime;
    notify_start();
    EXPECT_EQ(closes, 1);
    EXPECT_EQ(sockets, 2);
    EXPECT_EQ(g_report_pid, fakePid);
    EXPECT_EQ(g_notify_owner, 0);
}
