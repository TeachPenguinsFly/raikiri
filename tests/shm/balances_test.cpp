// The balance channel, and the one property that made it a single cell.
//
// A balance set is a SET: equity is the sum of it. A reader that took two rows
// from one snapshot and two from the next would add up a figure the account
// never held at any instant — so the whole set is one seqlock write, and these
// tests are mostly about that being true rather than about the numbers.

#include <raikiri/shm/records.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace raikiri::shm {
namespace {

constexpr std::uint64_t k_pull_ns = 1'000'000'000;

std::unique_ptr<BalanceTable> fresh_table() { return std::make_unique<BalanceTable>(); }

BalanceReport asset(std::string_view code, std::int64_t total) {
    BalanceReport out{};
    out.asset = *AssetCode::from(code);
    out.total = FixedE9::from_raw(total);
    return out;
}

TEST(BalanceTable, AnUnwrittenCellIsSilenceRatherThanAnEmptyAccount) {
    const auto table = fresh_table();
    BalanceSnapshot out{};
    // Version 0 is the seqlock saying nothing has ever been written here. A
    // caller that ignored it would read a zeroed snapshot with count 0 and
    // report an account holding nothing — which is what happens when a venue
    // cannot answer, and is not what it means.
    EXPECT_EQ(table->read(0, out), 0U);
    EXPECT_EQ(out.count, 0U);
    EXPECT_EQ(out.ts_pulled.ns, 0U);
}

TEST(BalanceTable, AWrittenEmptySetIsTheVenueSayingItHoldsNothing) {
    const auto table = fresh_table();
    BalanceSnapshot snapshot{};
    snapshot.count = 0;
    snapshot.ts_pulled = UnixNanos{k_pull_ns};
    table->write(0, snapshot);

    BalanceSnapshot out{};
    ASSERT_NE(table->read(0, out), 0U);
    EXPECT_EQ(out.count, 0U);
    // The timestamp is what separates this from the case above: same count,
    // different fact.
    EXPECT_EQ(out.ts_pulled.ns, k_pull_ns);
}

TEST(BalanceTable, TheWholeSetArrivesOrNoneOfItDoes) {
    const auto table = fresh_table();
    BalanceSnapshot first{};
    first.count = 2;
    first.ts_pulled = UnixNanos{k_pull_ns};
    first.rows[0] = asset("USDT", 100'000'000'000);
    first.rows[1] = asset("BTC", 2'000'000'000);
    table->write(0, first);

    BalanceSnapshot second{};
    second.count = 2;
    second.ts_pulled = UnixNanos{k_pull_ns * 2};
    second.rows[0] = asset("USDT", 900'000'000'000);
    second.rows[1] = asset("BTC", 9'000'000'000);
    table->write(0, second);

    BalanceSnapshot out{};
    ASSERT_NE(table->read(0, out), 0U);
    // Both rows from the SAME write. A row-at-a-time channel could hand back
    // the new USDT beside the old BTC, and their sum is an equity that existed
    // at no moment.
    EXPECT_EQ(out.rows[0].total.raw, 900'000'000'000);
    EXPECT_EQ(out.rows[1].total.raw, 9'000'000'000);
    EXPECT_EQ(out.ts_pulled.ns, k_pull_ns * 2);
}

TEST(BalanceTable, AShrinkingSetDoesNotLeaveTheDroppedAssetBehind) {
    const auto table = fresh_table();
    BalanceSnapshot two{};
    two.count = 2;
    two.ts_pulled = UnixNanos{k_pull_ns};
    two.rows[0] = asset("USDT", 100'000'000'000);
    two.rows[1] = asset("BTC", 2'000'000'000);
    table->write(0, two);

    BalanceSnapshot one{};
    one.count = 1;
    one.ts_pulled = UnixNanos{k_pull_ns * 2};
    one.rows[0] = asset("USDT", 100'000'000'000);
    table->write(0, one);

    BalanceSnapshot out{};
    ASSERT_NE(table->read(0, out), 0U);
    // `count` is what says which rows are live, and the writer replaces the
    // WHOLE snapshot rather than the rows it happens to have — so the dropped
    // coin is gone from the array too. Both halves matter: a writer that
    // updated in place would leave BTC sitting there for a reader that walked
    // the array instead of the count.
    EXPECT_EQ(out.count, 1U);
    EXPECT_FALSE(out.rows[1].has_asset());
    EXPECT_TRUE(out.rows[1].total.is_undef());
}

TEST(BalanceReportFields, UndefIsTheDefaultBecauseZeroWouldBeAClaim) {
    const BalanceReport fresh{};
    // Every figure starts undefined. A venue that reports a total and no free
    // figure leaves `available` here, and a reader must be able to tell that
    // from a venue reporting nothing free.
    EXPECT_TRUE(fresh.total.is_undef());
    EXPECT_TRUE(fresh.available.is_undef());
    EXPECT_TRUE(fresh.unrealized.is_undef());
    EXPECT_FALSE(fresh.has_asset());
}

} // namespace
} // namespace raikiri::shm
