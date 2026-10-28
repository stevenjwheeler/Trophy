/* Real architecture-matched C shared object, no GL driver or game. */
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <cstdlib>
#include <memory>
#include <string>

TEST(ReporterDlopenTest, DisabledGlSharedObjectLoadsAndExportsAllHooks) {
  const char *previous = std::getenv("DISABLE_TROPHY_DETECTION");
  const bool existed = previous != nullptr;
  const std::string saved = previous ? previous : "";
  struct Restore {
    bool existed;
    const std::string &saved;
    ~Restore() {
      if (existed) setenv("DISABLE_TROPHY_DETECTION", saved.c_str(), 1);
      else unsetenv("DISABLE_TROPHY_DETECTION");
    }
  } restore{existed, saved};
  ASSERT_EQ(setenv("DISABLE_TROPHY_DETECTION", "1", 1), 0);
  const auto close = [](void *handle) { if (handle) dlclose(handle); };
  std::unique_ptr<void, decltype(close)> library(dlopen(TROPHY_GL_SHIM_FILE, RTLD_NOW | RTLD_LOCAL), close);
  ASSERT_NE(library, nullptr) << dlerror();
  auto egl = reinterpret_cast<int (*)(void *, void *)>(dlsym(library.get(), "eglSwapBuffers"));
  auto glx = reinterpret_cast<void (*)(void *, unsigned long)>(dlsym(library.get(), "glXSwapBuffers"));
  auto sdl = reinterpret_cast<void (*)(void *)>(dlsym(library.get(), "SDL_GL_SwapWindow"));
  ASSERT_NE(egl, nullptr); ASSERT_NE(glx, nullptr); ASSERT_NE(sdl, nullptr);
  // No downstream graphics library exists in this runner. Missing EGL returns
  // false; GLX/SDL no-op. Reporting is disabled before any hook is invoked.
  EXPECT_EQ(egl(nullptr, nullptr), 0);
  glx(nullptr, 0);
  sdl(nullptr);
}
