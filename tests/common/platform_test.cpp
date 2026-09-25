#include <raikiri/common/platform.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace raikiri {
namespace {

TEST(Platform, SelfIsAlive) {
    EXPECT_TRUE(platform::process_alive(::getpid()));
    EXPECT_FALSE(platform::process_alive(-1));
}

TEST(Platform, PreallocateSetsFileSize) {
    char path[] = "/tmp/raikiri_prealloc_XXXXXX";
    const int fd = ::mkstemp(path);
    ASSERT_GE(fd, 0);

    constexpr std::uint64_t k_size = 1U << 20;
    const auto result = platform::preallocate(fd, k_size);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    struct stat st {};

    ASSERT_EQ(::fstat(fd, &st), 0);
    EXPECT_EQ(static_cast<std::uint64_t>(st.st_size), k_size);

    ::close(fd);
    ::unlink(path);
}

TEST(Platform, PinThreadBehavesPerPlatform) {
    const auto result = platform::pin_thread(0);
#if defined(__linux__)
    // may fail in constrained ci cgroups, but must not report ENOTSUP
    if (!result.has_value()) {
        EXPECT_NE(result.error().value(), ENOTSUP);
    }
#else
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), ENOTSUP);
#endif
}

TEST(Platform, RuntimeDirHonorsOverride) {
    ::setenv("RAIKIRI_RUNTIME_DIR", "/tmp/raikiri-test-rt", 1);
    EXPECT_EQ(platform::runtime_dir(), "/tmp/raikiri-test-rt");
    ::unsetenv("RAIKIRI_RUNTIME_DIR");
    EXPECT_FALSE(platform::runtime_dir().empty());
}

} // namespace
} // namespace raikiri
