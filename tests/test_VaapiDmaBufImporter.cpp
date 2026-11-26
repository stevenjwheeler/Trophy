/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <filesystem>
#include "encoding/VaapiDmaBufImporter.h"

using namespace TrophyBackend;
namespace {
constexpr uint64_t modifier = 0x0200000000000001ULL; // Opaque fixture, not real GPU evidence.
struct Fixture {
  int fd = memfd_create("trophy-prime2-test", MFD_CLOEXEC);
  VideoFrame frame{};
  Fixture() {
    if (fd >= 0) ftruncate(fd, 65536);
    frame.isDmaBuf = true;
    frame.width = 64; frame.height = 32;
    frame.drmFormat = DRM_FORMAT_XRGB8888;
    frame.drmModifier = modifier;
    frame.dmaBufPlanes.count = 2;
    frame.dmaBufPlanes.planes[0] = {fd, 256, 0, 0};
    frame.dmaBufPlanes.planes[1] = {fd, 64, 32768, 0};
  }
  ~Fixture() { close(fd); }
};
struct Fake {
  static inline int creates = 0, destroys = 0, fd = -1;
  static inline bool fail = false;
  static inline bool failDestroy = false;
  static inline VADRMPRIMESurfaceDescriptor descriptor{};
  static VAStatus create(VADisplay, unsigned int format, unsigned int width,
                         unsigned int height, VASurfaceID *out, unsigned int count,
                         VASurfaceAttrib *attrs, unsigned int nattrs) {
    ++creates;
    EXPECT_EQ(format, VA_RT_FORMAT_RGB32);
    EXPECT_EQ(width, 64u); EXPECT_EQ(height, 32u);
    EXPECT_EQ(count, 1u); EXPECT_EQ(nattrs, 2u);
    EXPECT_EQ(attrs[0].value.value.i, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);
    descriptor = *static_cast<VADRMPRIMESurfaceDescriptor *>(attrs[1].value.value.p);
    fd = descriptor.objects[0].fd;
    EXPECT_GE(fcntl(fd, F_GETFD), 0);
    if (fail) return VA_STATUS_ERROR_OPERATION_FAILED;
    *out = 42;
    return VA_STATUS_SUCCESS;
  }
  static VAStatus destroy(VADisplay, VASurfaceID *surface, int count) {
    ++destroys;
    EXPECT_EQ(*surface, 42u); EXPECT_EQ(count, 1);
    // Source handles must still be owned while destroying the native surface.
    EXPECT_GE(fcntl(fd, F_GETFD), 0);
    return failDestroy ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
  }
  static VaapiDmaBufImporter::Ops ops() {
    creates = destroys = 0; fail = failDestroy = false; fd = -1;
    return {create, destroy};
  }
};
} // namespace

TEST(VaapiDmaBufImporterTest, DescriptorPreservesSameAllocationPlanesAndActualSize) {
  Fixture f;
  const int second = dup(f.fd);
  ASSERT_GE(second, 0);
  f.frame.dmaBufPlanes.planes[1].fd = second;
  VADRMPRIMESurfaceDescriptor desc{};
  QString error;
  ASSERT_TRUE(VaapiDmaBufImporter::describe(f.frame, desc, error)) << error.toStdString();
  EXPECT_EQ(desc.fourcc, VA_FOURCC_BGRX);
  EXPECT_EQ(desc.num_objects, 1u);
  EXPECT_EQ(desc.objects[0].size, 65536u);
  EXPECT_EQ(desc.objects[0].drm_format_modifier, modifier);
  EXPECT_EQ(desc.layers[0].drm_format, DRM_FORMAT_XRGB8888);
  EXPECT_EQ(desc.layers[0].num_planes, 2u);
  EXPECT_EQ(desc.layers[0].object_index[1], 0u);
  EXPECT_EQ(desc.layers[0].offset[1], 32768u);
  EXPECT_EQ(desc.layers[0].pitch[1], 64u);
  close(second);
}

TEST(VaapiDmaBufImporterTest, RejectsSeparateObjectsAndMalformedLayouts) {
  Fixture f, other;
  const auto original = f.frame;
  VADRMPRIMESurfaceDescriptor desc{};
  QString error;
  f.frame.dmaBufPlanes.planes[1].fd = other.fd;
  EXPECT_FALSE(VaapiDmaBufImporter::describe(f.frame, desc, error));
  EXPECT_TRUE(error.contains("Multi-object"));
  for (int kind = 0; kind < 7; ++kind) {
    SCOPED_TRACE(kind);
    f.frame = original;
    switch (kind) {
    case 0: f.frame.dmaBufPlanes.count = 5; break;
    case 1: f.frame.dmaBufPlanes.planes[1].fd = -1; break;
    case 2: f.frame.dmaBufPlanes.planes[1].offset = 65536; break;
    case 3: f.frame.dmaBufPlanes.planes[1].stride = -1; break;
    case 4: f.frame.drmModifier = DRM_FORMAT_MOD_INVALID; break;
    case 5: f.frame.drmModifier = DRM_FORMAT_MOD_LINEAR; break;
    case 6: f.frame.width = 0; break;
    }
    EXPECT_FALSE(VaapiDmaBufImporter::describe(f.frame, desc, error));
  }
}

TEST(VaapiDmaBufImporterTest, UnknownAllocationSizeDoesNotBecomeAnEstimatedCompressedBuffer) {
  Fixture f;
  ASSERT_EQ(ftruncate(f.fd, 0), 0);
  VADRMPRIMESurfaceDescriptor desc{};
  QString error;
  EXPECT_FALSE(VaapiDmaBufImporter::describe(f.frame, desc, error));
}

TEST(VaapiDmaBufImporterTest, NativeSurfaceIsReleasedBeforeOwnedHandlesAndLease) {
  Fixture f;
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> weak = lease;
  f.frame.bufferLease = lease;
  {
    VaapiDmaBufImporter importer(Fake::ops());
    ASSERT_TRUE(importer.import(reinterpret_cast<VADisplay>(1), f.frame));
    EXPECT_EQ(importer.surface(), 42u);
    f.frame.bufferLease.reset(); lease.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(Fake::destroys, 0);
    EXPECT_EQ(Fake::descriptor.layers[0].num_planes, 2u);
  }
  EXPECT_EQ(Fake::creates, 1);
  EXPECT_EQ(Fake::destroys, 1);
  EXPECT_TRUE(weak.expired());
  EXPECT_LT(fcntl(Fake::fd, F_GETFD), 0);
}

TEST(VaapiDmaBufImporterTest, RejectedPrime2NeverRetriesLegacyAndReleasesOwnership) {
  Fixture f;
  VaapiDmaBufImporter importer(Fake::ops());
  Fake::fail = true;
  EXPECT_FALSE(importer.import(reinterpret_cast<VADisplay>(1), f.frame));
  EXPECT_EQ(Fake::creates, 1);
  EXPECT_EQ(Fake::destroys, 0);
  EXPECT_NE(importer.error().indexOf("PRIME_2"), -1);
  EXPECT_LT(fcntl(Fake::fd, F_GETFD), 0);
}

TEST(VaapiDmaBufImporterTest, FailedNativeCleanupRetainsOneSurfaceAndLeaseUntilRetrySucceeds) {
  Fixture f;
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> weak = lease;
  f.frame.bufferLease = lease;
  VaapiDmaBufImporter importer(Fake::ops());
  ASSERT_TRUE(importer.import(reinterpret_cast<VADisplay>(1), f.frame));
  const int owned = Fake::fd;
  f.frame.bufferLease.reset(); lease.reset();
  Fake::failDestroy = true;
  EXPECT_FALSE(importer.release());
  EXPECT_FALSE(weak.expired());
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  for (int i = 0; i < 10; ++i)
    EXPECT_FALSE(importer.import(reinterpret_cast<VADisplay>(1), f.frame));
  EXPECT_EQ(Fake::creates, 1); // No accumulating source surfaces on cleanup errors.
  EXPECT_FALSE(importer.error().isEmpty());
  Fake::failDestroy = false;
  EXPECT_TRUE(importer.release());
  EXPECT_TRUE(weak.expired());
  EXPECT_LT(fcntl(owned, F_GETFD), 0);
}

TEST(VaapiDmaBufImporterTest, RepeatedTwoAndFourPlaneImportsAndFailuresHaveNoFdGrowth) {
  Fixture f;
  for (uint32_t i = 2; i < 4; ++i)
    f.frame.dmaBufPlanes.planes[i] = {f.fd, 64, 32768 + i * 4096, 0};
  VaapiDmaBufImporter importer(Fake::ops());
  auto fdCount = [] {
    size_t result = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      (void)entry;
      ++result;
    }
    return result;
  };
  const auto baseline = fdCount();
  int successes = 0;
  for (int i = 0; i < 500; ++i) {
    f.frame.dmaBufPlanes.count = (i & 1) ? 4 : 2;
    Fake::fail = i % 7 == 0;
    EXPECT_EQ(importer.import(reinterpret_cast<VADisplay>(1), f.frame), !Fake::fail);
    if (!Fake::fail) {
      ++successes;
      EXPECT_EQ(Fake::descriptor.layers[0].num_planes, f.frame.dmaBufPlanes.count);
    }
    EXPECT_TRUE(importer.release());
    EXPECT_EQ(importer.surface(), VA_INVALID_SURFACE);
    EXPECT_EQ(fdCount(), baseline);
  }
  EXPECT_EQ(Fake::creates, 500);
  EXPECT_EQ(Fake::destroys, successes);
}
