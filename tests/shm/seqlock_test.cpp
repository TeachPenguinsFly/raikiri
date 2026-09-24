#include <raikiri/shm/records.hpp>
#include <raikiri/shm/seqlock_table.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

namespace raikiri::shm {
namespace {

struct Snapshot {
    std::uint64_t a{0};
    std::uint64_t b{0};
    std::uint64_t c{0};

    // writers keep a == b == c
    [[nodiscard]] bool coherent() const noexcept { return a == b && b == c; }
};

TEST(FundingTable, ContinuousAccrualCrossesTheCell) {
    const auto table = std::make_unique<FundingTable>();
    FundingUpdate update{};
    update.interval_s = 28'800;
    update.accrual = FundingAccrual::Continuous;
    table->write(3, update);

    FundingUpdate out{};
    ASSERT_NE(table->read(3, out), 0U);
    EXPECT_EQ(out.accrual, FundingAccrual::Continuous);
    EXPECT_EQ(out.next_funding_time.ns, 0U);
    EXPECT_EQ(out.interval_s, 28'800U);
}

TEST(Seqlock, UnwrittenCellReadsAsVersionZero) {
    SeqlockCell<Snapshot> cell{};
    Snapshot out{};
    EXPECT_EQ(cell.read(out), 0U);
}

TEST(Seqlock, WriteThenReadRoundTrips) {
    SeqlockCell<Snapshot> cell{};
    cell.write({7, 7, 7});
    Snapshot out{};
    const auto v = cell.read(out);
    EXPECT_NE(v, 0U);
    EXPECT_EQ(v % 2, 0U);
    EXPECT_EQ(out.a, 7U);
    EXPECT_TRUE(out.coherent());
}

TEST(Seqlock, VersionAdvancesPerWriteForChangeGating) {
    SeqlockCell<Snapshot> cell{};
    cell.write({1, 1, 1});
    Snapshot out{};
    const auto v1 = cell.read(out);
    cell.write({2, 2, 2});
    const auto v2 = cell.read(out);
    EXPECT_GT(v2, v1); // reader gates on "version changed since last seen"
    EXPECT_EQ(cell.version_now(), v2);
}

TEST(SeqlockStress, ReadersNeverObserveTornSnapshots) {
    auto cell = std::make_unique<SeqlockCell<Snapshot>>();
    std::atomic<bool> stop{false};

    std::thread writer{[&] {
        for (std::uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
            cell->write({i, i, i});
        }
    }};

    constexpr int k_readers = 3;
    std::vector<std::thread> readers;
    readers.reserve(k_readers);
    std::atomic<std::uint64_t> reads_done{0};
    for (int r = 0; r < k_readers; ++r) {
        readers.emplace_back([&] {
            Snapshot out{};
            std::uint64_t last_version = 0;
            for (int i = 0; i < 200'000; ++i) {
                const auto v = cell->read(out);
                if (v != 0) {
                    ASSERT_TRUE(out.coherent()) << out.a << ' ' << out.b << ' ' << out.c;
                    ASSERT_GE(v, last_version); // versions never regress
                    last_version = v;
                }
            }
            reads_done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : readers) {
        t.join();
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    EXPECT_EQ(reads_done.load(), k_readers);
}

} // namespace
} // namespace raikiri::shm
