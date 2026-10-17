/*
 * Trophy - OpenGL detection shim
 * LD_PRELOAD library that reports OpenGL games to Trophy.
 *
 * Same socket protocol as the Vulkan layer (src/vulkan_layer/trophy_layer.cpp):
 *   START:<pid>:<name>[:<steamAppId>]
 *   STOP:<pid>
 * on /tmp/trophy-detection.sock
 *
 * Hooks: glXSwapBuffers (XWayland/GLX), eglSwapBuffers (native Wayland EGL),
 *        SDL_GL_SwapWindow (SDL2 games that hide GL behind SDL).
 *
 * Design constraints (preload hot path):
 *  - C only, no C++ / malloc / printf in swap path after first notify.
 *  - report once per connection, with rate-limited nonblocking reconnects.
 *  - never call GL from inside the shim (no recursion).
 *  - DISABLE_TROPHY_DETECTION=1 opts out; Trophy itself is skipped.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

#define SOCKET_PATH "/tmp/trophy-detection.sock"

typedef void (*glx_swap_fn)(void *, unsigned long);
typedef int (*egl_swap_fn)(void *, void *);
typedef void (*sdl_swap_fn)(void *);
static glx_swap_fn g_real_glx_swap;
static egl_swap_fn g_real_egl_swap;
static sdl_swap_fn g_real_sdl_swap;

static int g_sock = -1; /* every access via __atomic_* below */
 /* 0 = not reported, 1 = initializing or deliberately disabled, 2 = live. */
static int g_notified = 0; /* every access via __atomic_* below */
static int g_disabled = -1; /* -1 unknown, 0 no, 1 yes; __atomic_* only */
static pid_t g_report_pid = 0; /* PID owning g_sock/g_notified; fork guard */
static long g_last_health_check = 0; /* monotonic seconds */
static long g_last_connect_attempt = 0; /* monotonic seconds */
/* Never wait for another render thread. The PID-valued guard is recoverable
 * after fork, even when the parent forked during a notification. */
static pid_t g_notify_owner = 0;

static int claim_second(long *last, long now) {
    long observed = __atomic_load_n(last, __ATOMIC_RELAXED);
    while (observed != now) {
        if (__atomic_compare_exchange_n(last, &observed, now, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

static int socket_is_alive(int fd) {
    char byte;
    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n > 0 ||
           (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR));
}

/* --- helpers (only run once, not per-frame) --- */

static void app_basename(char *buf, size_t cap);

static int is_disabled(void) {
    int cached = __atomic_load_n(&g_disabled, __ATOMIC_ACQUIRE);
    if (cached != -1)
        return cached;
    const char *e = getenv("DISABLE_TROPHY_DETECTION");
    if (e && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0')) {
        __atomic_store_n(&g_disabled, 1, __ATOMIC_RELEASE);
        return 1;
    }
    /* Never report Trophy itself. /proc/self/comm is the calling *thread's*
     * name, so a first swap off the main thread never matches — check the
     * process exe basename instead (comm kept as a fallback). */
    char app[128];
    app_basename(app, sizeof(app));
    if (strcmp(app, "trophy") == 0 || strcmp(app, "TrophyApp") == 0) {
        __atomic_store_n(&g_disabled, 1, __ATOMIC_RELEASE);
        return 1;
    }
    char comm[64];
    int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n > 0) {
            comm[n] = '\0';
            /* strip trailing newline */
            char *nl = strchr(comm, '\n');
            if (nl)
                *nl = '\0';
            if (strcmp(comm, "trophy") == 0 ||
                strcmp(comm, "TrophyApp") == 0) {
                __atomic_store_n(&g_disabled, 1, __ATOMIC_RELEASE);
                return 1;
            }
        }
    }
    __atomic_store_n(&g_disabled, 0, __ATOMIC_RELEASE);
    return 0;
}

/* basename of /proc/self/exe into buf (always NUL-terminated). */
static void app_basename(char *buf, size_t cap) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        strncpy(buf, "Unknown GL Game", cap);
        buf[cap - 1] = '\0';
        return;
    }
    exe[n] = '\0';
    char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    /* strip version suffixes some launchers append? keep as-is, service normalizes */
    snprintf(buf, cap, "%s", base);
    if (buf[0] == '\0')
        snprintf(buf, cap, "%s", "Unknown GL Game");
}

static void notify_start_locked(void) {
    /* Fork safety: g_sock/g_notified are inherited across fork(). A forked
     * child (new PID) must re-report with its own PID, never reuse the
     * parent's connection or stay silent on an inherited "live" flag.
     * (fork+exec resets everything anyway; plain fork keeps the exe, so the
     * cached g_disabled stays valid.) */
    pid_t self = getpid();
    if (__atomic_load_n(&g_report_pid, __ATOMIC_ACQUIRE) != self) {
        int inherited = __atomic_exchange_n(&g_sock, -1, __ATOMIC_ACQ_REL);
        if (inherited >= 0)
            close(inherited);
        __atomic_store_n(&g_notified, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_last_health_check, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_last_connect_attempt, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_report_pid, self, __ATOMIC_RELEASE);
    }
    int state = __atomic_load_n(&g_notified, __ATOMIC_ACQUIRE);
    if (state == 2) {
        // Already reported: cheap once-per-second health check so a
        // Trophy restart is noticed and the next swap re-reports.
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (!claim_second(&g_last_health_check, ts.tv_sec))
            return;
        int sock = __atomic_load_n(&g_sock, __ATOMIC_ACQUIRE);
        if (sock < 0 || !socket_is_alive(sock)) {
            int old = __atomic_exchange_n(&g_sock, -1, __ATOMIC_ACQ_REL);
            if (old >= 0)
                close(old);
            __atomic_store_n(&g_notified, 0, __ATOMIC_RELEASE);
        }
        return;
    }
    if (state == 1)
        return; /* another thread is connecting, or detection is disabled */

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // Retry a failed initial connection at most once per second instead of
    // opening a socket on every swap while Trophy is not running.
    if (!claim_second(&g_last_connect_attempt, ts.tv_sec))
        return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_notified, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    if (is_disabled())
        return;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        __atomic_store_n(&g_notified, 0, __ATOMIC_RELEASE);
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        __atomic_store_n(&g_notified, 0, __ATOMIC_RELEASE);
        return;
    }
    /* Kept open until unload so STOP can reuse it. */
    __atomic_store_n(&g_sock, fd, __ATOMIC_RELEASE);

    char app[128];
    app_basename(app, sizeof(app));
    for (char *p = app; *p; ++p)
        if (*p == '\n' || *p == '\r')
            *p = ' ';

    const char *appid = getenv("SteamAppId");
    if (!appid || !*appid)
        appid = "0";
    /* Never let inherited environment data inject another protocol line. */
    size_t appid_len = strnlen(appid, 11);
    if (appid_len == 0 || appid_len > 10 ||
        strspn(appid, "0123456789") != appid_len ||
        strtoull(appid, NULL, 10) > UINT32_MAX)
        appid = "0";

    /* Single stack buffer, single send: "START:<pid>:<app>:<appid>\n" */
    char msg[256];
    int len = snprintf(msg, sizeof(msg), "START:%d:%s:%s\n", (int)self, app,
                       appid);
    if (len > 0) {
        if ((size_t)len >= sizeof(msg)) {
            len = (int)sizeof(msg) - 1;
            /* Clamping cut the trailing '\n'; the server only fires on full
             * lines, so restore it or notify-once never retries. */
            msg[len - 1] = '\n';
        }
        if (send(fd, msg, (size_t)len, MSG_NOSIGNAL | MSG_DONTWAIT) != len) {
            close(fd);
            __atomic_store_n(&g_sock, -1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_notified, 0, __ATOMIC_RELEASE);
            return;
        }
    }
    __atomic_store_n(&g_notified, 2, __ATOMIC_RELEASE);
}

static void notify_start(void) {
    const int saved_errno = errno;
    const pid_t self = getpid();
    pid_t owner = __atomic_load_n(&g_notify_owner, __ATOMIC_ACQUIRE);
    if (owner != self &&
        __atomic_compare_exchange_n(&g_notify_owner, &owner, self, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        notify_start_locked();
        __atomic_store_n(&g_notify_owner, 0, __ATOMIC_RELEASE);
    }
    errno = saved_errno;
}

__attribute__((destructor)) static void trophy_gl_unload(void) {
    /* A forked child that never reported must not send STOP for the parent:
     * just drop the inherited socket copy. */
    if (__atomic_load_n(&g_report_pid, __ATOMIC_ACQUIRE) != getpid()) {
        int inherited = __atomic_exchange_n(&g_sock, -1, __ATOMIC_ACQ_REL);
        if (inherited >= 0)
            close(inherited);
        return;
    }
    if (__atomic_load_n(&g_notified, __ATOMIC_ACQUIRE) == 0)
        return;
    int sock = __atomic_exchange_n(&g_sock, -1, __ATOMIC_ACQ_REL);
    if (sock < 0)
        return;
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "STOP:%d\n", (int)getpid());
    if (len > 0) {
        if ((size_t)len >= sizeof(msg))
            len = (int)sizeof(msg) - 1;
        (void)send(sock, msg, (size_t)len, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    close(sock);
}

/* --- interposed entry points (ABI-compatible, header-free) --- */

void glXSwapBuffers(void *dpy, unsigned long drawable) {
    const int saved_errno = errno;
    glx_swap_fn real_fn = __atomic_load_n(&g_real_glx_swap, __ATOMIC_ACQUIRE);
    if (!real_fn)
        real_fn = (glx_swap_fn)dlsym(RTLD_NEXT, "glXSwapBuffers");
    if (real_fn)
        __atomic_store_n(&g_real_glx_swap, real_fn, __ATOMIC_RELEASE);
    errno = saved_errno;
    if (real_fn)
        notify_start();
    if (real_fn)
        real_fn(dpy, drawable);
}

int eglSwapBuffers(void *dpy, void *surf) {
    const int saved_errno = errno;
    egl_swap_fn real_fn = __atomic_load_n(&g_real_egl_swap, __ATOMIC_ACQUIRE);
    if (!real_fn)
        real_fn = (egl_swap_fn)dlsym(RTLD_NEXT, "eglSwapBuffers");
    if (real_fn)
        __atomic_store_n(&g_real_egl_swap, real_fn, __ATOMIC_RELEASE);
    errno = saved_errno;
    if (real_fn)
        notify_start();
    if (real_fn)
        return real_fn(dpy, surf);
    return 0; /* EGL_FALSE: never claim a swap happened without EGL. */
}

void SDL_GL_SwapWindow(void *window) {
    const int saved_errno = errno;
    sdl_swap_fn real_fn = __atomic_load_n(&g_real_sdl_swap, __ATOMIC_ACQUIRE);
    if (!real_fn)
        real_fn = (sdl_swap_fn)dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    if (real_fn)
        __atomic_store_n(&g_real_sdl_swap, real_fn, __ATOMIC_RELEASE);
    errno = saved_errno;
    if (real_fn)
        notify_start();
    if (real_fn)
        real_fn(window);
}
