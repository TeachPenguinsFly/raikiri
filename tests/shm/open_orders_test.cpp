#include <raikiri/shm/records.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace raikiri::shm {
namespace {

OpenOrderCell row(std::string_view id, UnixNanos pulled) {
    OpenOrderCell out{};
    out.exchange_client_order_id = *ExchangeClientOrderId::from(id);
    out.ts_pulled = pulled;
    return out;
}

std::unique_ptr<OpenOrderTable> table_of() {
    return std::make_unique<OpenOrderTable>();
}

TEST(OpenOrders, NothingPublishedIsNotAnEmptyVenue) {
    const auto table = table_of();
    EXPECT_FALSE(read_open_orders(*table).has_value());
}

TEST(OpenOrders, AWholePullReadsBack) {
    const auto table = table_of();
    const UnixNanos pulled{100};
    const std::vector rows{row("A", pulled), row("B", pulled)};
    publish_open_orders(*table, rows, pulled);
    const auto read = read_open_orders(*table);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->pulled, pulled);
    ASSERT_EQ(read->rows.size(), 2U);
    EXPECT_EQ(read->rows[1].exchange_client_order_id.view(), "B");
}

TEST(OpenOrders, AVenueWithNoOrdersAnswersEmptyNotUnknown) {
    const auto table = table_of();
    const UnixNanos pulled{100};
    publish_open_orders(*table, {}, pulled);
    const auto read = read_open_orders(*table);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(read->rows.empty());
}

TEST(OpenOrders, AShorterPullLeavesNoRowOfTheLongerOneBehind) {
    const auto table = table_of();
    const std::vector first{row("A", UnixNanos{100}), row("B", UnixNanos{100})};
    publish_open_orders(*table, first, UnixNanos{100});
    const std::vector second{row("B", UnixNanos{200})};
    publish_open_orders(*table, second, UnixNanos{200});
    const auto read = read_open_orders(*table);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->rows.size(), 1U);
    EXPECT_EQ(read->rows[0].exchange_client_order_id.view(), "B");
}

// the old reader took cells one by one here and saw B gone, so recon closed a live order
TEST(OpenOrders, AReaderInsideAPullReadsNothing) {
    const auto table = table_of();
    const std::vector first{row("A", UnixNanos{100}), row("B", UnixNanos{100})};
    publish_open_orders(*table, first, UnixNanos{100});
    table->write(k_open_order_cells - 1, OpenOrderCell{});
    table->write(0, row("B", UnixNanos{200}));
    EXPECT_FALSE(read_open_orders(*table).has_value());
}

// a row from another pull under a marker that looks finished
TEST(OpenOrders, AMixOfTwoPullsReadsNothing) {
    const auto table = table_of();
    const std::vector first{row("A", UnixNanos{100}), row("B", UnixNanos{100})};
    publish_open_orders(*table, first, UnixNanos{100});
    table->write(1, row("B", UnixNanos{200}));
    EXPECT_FALSE(read_open_orders(*table).has_value());
}

TEST(OpenOrders, TwoOrdersSwappingSlotsAreNeverSeenMissing) {
    const auto table = table_of();
    std::atomic<bool> stop{false};
    publish_open_orders(*table, std::vector{row("A", UnixNanos{1}), row("B", UnixNanos{1})}, UnixNanos{1});
    std::thread writer{[&] {
        std::uint64_t now = 2;
        while (!stop.load(std::memory_order_relaxed)) {
            const UnixNanos pulled{now};
            const auto first = (now % 2) == 0 ? "A" : "B";
            const auto second = (now % 2) == 0 ? "B" : "A";
            publish_open_orders(*table, std::vector{row(first, pulled), row(second, pulled)}, pulled);
            ++now;
        }
    }};
    std::uint64_t whole = 0;
    for (int i = 0; i < 2000; ++i) {
        if (const auto read = read_open_orders(*table); read.has_value()) {
            ++whole;
            ASSERT_EQ(read->rows.size(), 2U);
            EXPECT_NE(read->rows[0].exchange_client_order_id.view(), read->rows[1].exchange_client_order_id.view());
        }
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    SUCCEED() << whole << " whole reads";
}

} // namespace
} // namespace raikiri::shm
