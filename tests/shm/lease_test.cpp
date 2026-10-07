#include <raikiri/shm/lease.hpp>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <string>

namespace raikiri::shm {
namespace {

std::string test_path(const char* name) {
    return "/tmp/raikiri-leasetest-" + std::to_string(::getpid()) + "-" + name;
}

class LeaseFile : public ::testing::Test {
protected:
    void TearDown() override {
        for (const auto role : {LeaseRole::Strategy, LeaseRole::Desk}) {
            ::unlink(lease_path(path_, role).c_str());
        }
    }

    std::string path_ = test_path(::testing::UnitTest::GetInstance()->current_test_info()->name());
};

// forks a child that claims the lease, reports, then waits to be killed or exits on its own
pid_t spawn_holder(const std::string& path, LeaseRole role, bool exit_after_claim, int& ready_fd) {
    int fds[2];
    if (::pipe(fds) != 0) {
        return -1;
    }
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(fds[0]);
        auto lease = WriterLease::claim(path, role);
        const char ok = lease.has_value() ? 'y' : 'n';
        if (::write(fds[1], &ok, 1) != 1) {
            ::_exit(3);
        }
        if (exit_after_claim) {
            ::_exit(0);
        }
        for (;;) {
            ::pause();
        }
    }
    ::close(fds[1]);
    ready_fd = fds[0];
    return child;
}

TEST(LeasePath, SitsBesideTheSegmentOutsideItsPrefix) {
    EXPECT_EQ(lease_path("/run/raikiri/shm/gw.binance-perp", LeaseRole::Strategy),
              "/run/raikiri/shm/lease.gw.binance-perp.strategy");
    EXPECT_EQ(lease_path("gw.okx-spot", LeaseRole::Desk), "./lease.gw.okx-spot.desk");
}

TEST_F(LeaseFile, SecondProducerRefusedInOneProcess) {
    auto first = WriterLease::claim(path_, LeaseRole::Strategy);
    ASSERT_TRUE(first.has_value());

    const auto second = WriterLease::claim(path_, LeaseRole::Strategy);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().error, LeaseError::Held);
    EXPECT_EQ(second.error().holder_pid, ::getpid());

    EXPECT_TRUE(WriterLease::claim(path_, LeaseRole::Desk).has_value()) << "roles lease independently";

    first->release();
    EXPECT_FALSE(first->held());
    EXPECT_TRUE(WriterLease::claim(path_, LeaseRole::Strategy).has_value());
}

TEST_F(LeaseFile, DestructorAndMoveRelease) {
    {
        auto lease = WriterLease::claim(path_, LeaseRole::Desk);
        ASSERT_TRUE(lease.has_value());
        WriterLease moved{std::move(*lease)};
        EXPECT_FALSE(lease->held());
        EXPECT_TRUE(moved.held());
        EXPECT_FALSE(WriterLease::claim(path_, LeaseRole::Desk).has_value());
    }
    EXPECT_TRUE(WriterLease::claim(path_, LeaseRole::Desk).has_value());
}

TEST_F(LeaseFile, KilledHolderReleases) {
    int ready = -1;
    const pid_t child = spawn_holder(path_, LeaseRole::Strategy, /*exit_after_claim=*/false, ready);
    ASSERT_GT(child, 0);
    char ok = 0;
    ASSERT_EQ(::read(ready, &ok, 1), 1);
    ::close(ready);
    ASSERT_EQ(ok, 'y');

    const auto refused = WriterLease::claim(path_, LeaseRole::Strategy);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().error, LeaseError::Held);
    EXPECT_EQ(refused.error().holder_pid, child);

    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_TRUE(WriterLease::claim(path_, LeaseRole::Strategy).has_value());
}

TEST_F(LeaseFile, CleanExitReleases) {
    int ready = -1;
    const pid_t child = spawn_holder(path_, LeaseRole::Desk, /*exit_after_claim=*/true, ready);
    ASSERT_GT(child, 0);
    char ok = 0;
    ASSERT_EQ(::read(ready, &ok, 1), 1);
    ::close(ready);
    ASSERT_EQ(ok, 'y');
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_TRUE(WriterLease::claim(path_, LeaseRole::Desk).has_value());
}

TEST_F(LeaseFile, ChildRefusedWhileParentHolds) {
    auto held = WriterLease::claim(path_, LeaseRole::Strategy);
    ASSERT_TRUE(held.has_value());
    int ready = -1;
    const pid_t child = spawn_holder(path_, LeaseRole::Strategy, /*exit_after_claim=*/true, ready);
    ASSERT_GT(child, 0);
    char ok = 0;
    ASSERT_EQ(::read(ready, &ok, 1), 1);
    ::close(ready);
    EXPECT_EQ(ok, 'n');
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_FALSE(WriterLease::claim(path_, LeaseRole::Strategy).has_value()) << "the child's exit must not free it";
}

} // namespace
} // namespace raikiri::shm
