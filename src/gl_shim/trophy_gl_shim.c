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
 *  - notify exactly once per process (atomic CAS), then straight through.
 *  - never call GL from inside the shim (no recursion).
 *  - DISABLE_TROPHY_DETECTION=1 opts out; Trophy itself is skipped.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
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

static int g_sock = -1;
static int g_notified = 0;
static int g_disabled = -1; /* -1 unknown, 0 no, 1 yes */
static long g_last_health_check = 0; /* monotonic seconds */

/* --- helpers (only run once, not per-frame) --- */

static int is_disabled(void) {
    if (g_disabled != -1)
        return g_disabled;
    const char *e = getenv("DISABLE_TROPHY_DETECTION");
    if (e && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0')) {
        g_disabled = 1;
        return 1;
    }
    /* Never report Trophy itself */
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
                g_disabled = 1;
                return 1;
            }
        }
    }
    g_disabled = 0;
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

static void notify_start(void) {
    if (__atomic_load_n(&g_notified, __ATOMIC_ACQUIRE) != 0) {
        // Already reported: cheap once-per-second health check so a
        // Trophy restart is noticed and the next swap re-reports.
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec == g_last_health_check)
            return;
        g_last_health_check = ts.tv_sec;
        if (g_sock >= 0 && send(g_sock, "", 0, MSG_NOSIGNAL) != 0) {
            close(g_sock);
            g_sock = -1;
            __atomic_store_n(&g_notified, 0, __ATOMIC_RELEASE);
        }
        return;
    }
    if (__atomic_exchange_n(&g_notified, 1, __ATOMIC_ACQ_REL) != 0)
        return; /* lost the race; winner reports */
    if (is_disabled())
        return;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    g_sock = fd; /* kept open until unload so STOP can reuse it */

    char app[128];
    app_basename(app, sizeof(app));

    const char *appid = getenv("SteamAppId");
    if (!appid || !*appid)
        appid = "0";

    /* Single stack buffer, single send: "START:<pid>:<app>:<appid>\n" */
    char msg[256];
    pid_t pid = getpid();
    /* manual integer formatting to avoid snprintf locale/malloc concerns? snprintf is fine here (once per process). */
    int len = snprintf(msg, sizeof(msg), "START:%d:%s:%s\n", (int)pid, app, appid);
    if (len > 0) {
        if ((size_t)len >= sizeof(msg))
            len = (int)sizeof(msg) - 1;
        (void)send(fd, msg, (size_t)len, MSG_NOSIGNAL);
    }
}

__attribute__((destructor)) static void trophy_gl_unload(void) {
    if (g_notified == 0 || g_sock < 0)
        return;
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "STOP:%d\n", (int)getpid());
    if (len > 0) {
        if ((size_t)len >= sizeof(msg))
            len = (int)sizeof(msg) - 1;
        (void)send(g_sock, msg, (size_t)len, MSG_NOSIGNAL);
    }
    close(g_sock);
    g_sock = -1;
}

/* --- interposed entry points (ABI-compatible, header-free) --- */

void glXSwapBuffers(void *dpy, unsigned long drawable) {
    typedef void (*fn_t)(void *, unsigned long);
    static fn_t real_fn = NULL;
    if (!real_fn)
        real_fn = (fn_t)dlsym(RTLD_NEXT, "glXSwapBuffers");
    notify_start();
    if (real_fn)
        real_fn(dpy, drawable);
}

int eglSwapBuffers(void *dpy, void *surf) {
    typedef int (*fn_t)(void *, void *);
    static fn_t real_fn = NULL;
    if (!real_fn)
        real_fn = (fn_t)dlsym(RTLD_NEXT, "eglSwapBuffers");
    notify_start();
    if (real_fn)
        return real_fn(dpy, surf);
    return 1; /* EGL_TRUE fallback when no real EGL present (tests) */
}

void SDL_GL_SwapWindow(void *window) {
    typedef void (*fn_t)(void *);
    static fn_t real_fn = NULL;
    if (!real_fn)
        real_fn = (fn_t)dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    notify_start();
    if (real_fn)
        real_fn(window);
}
