#include <raikiri/venues/bybit/book_sync.hpp>

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace raikiri::venues::bybit {
namespace {

constexpr auto k_instrument_id = [] {
    return InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{0});
};

BookSync make_book() {
    return BookSync{k_instrument_id(), *parse_decimal_e9("0.1"), *parse_decimal_e9("0.001")};
}

WireOrderbook snapshot(std::span<const WireLevel> bids, std::span<const WireLevel> asks,
                       std::uint64_t update_id) {
    WireOrderbook ob{};
    ob.symbol = "BTCUSDT";
    ob.is_snapshot = true;
    ob.update_id = update_id;
    ob.seq = update_id * 10;
    ob.bids = bids;
    ob.asks = asks;
    return ob;
}

WireOrderbook delta(std::span<const WireLevel> bids, std::span<const WireLevel> asks,
                    std::uint64_t update_id) {
    auto ob = snapshot(bids, asks, update_id);
    ob.is_snapshot = false;
    return ob;
}

TEST(BookSync, SnapshotEstablishesTopOfBook) {
    auto book = make_book();
    EXPECT_FALSE(book.synced());

    const std::array bids{WireLevel{"16493.5", "0.006"}, WireLevel{"16493.0", "0.100"}};
    const std::array asks{WireLevel{"16611.0", "0.029"}, WireLevel{"16612.0", "0.213"}};
    EXPECT_EQ(book.apply(snapshot(bids, asks, 100)), SyncResult::Resynced);
    EXPECT_TRUE(book.synced());

    const auto bbo = book.bbo(UnixNanos{1}, UnixNanos{2});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 164935); // 16493.5 / 0.1
    EXPECT_EQ(bbo->ask.raw, 166110);
    EXPECT_EQ(bbo->bid_qty.raw, 6);  // 0.006 / 0.001
    EXPECT_EQ(bbo->ask_qty.raw, 29);
}

TEST(BookSync, DeltaUpdatesInsertsAndDeletes) {
    auto book = make_book();
    const std::array bids{WireLevel{"16493.5", "0.006"}, WireLevel{"16493.0", "0.100"}};
    const std::array asks{WireLevel{"16611.0", "0.029"}};
    ASSERT_EQ(book.apply(snapshot(bids, asks, 100)), SyncResult::Resynced);

    // Size "0" is bybit's delete; the best bid must fall through to the
    // next level rather than leaving a phantom.
    const std::array d_bids{WireLevel{"16493.5", "0"}, WireLevel{"16494.0", "0.050"}};
    const std::array d_asks{WireLevel{"16611.0", "0.030"}};
    EXPECT_EQ(book.apply(delta(d_bids, d_asks, 101)), SyncResult::Applied);

    const auto bbo = book.bbo(UnixNanos{1}, UnixNanos{2});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 164940) << "new higher level became the touch";
    EXPECT_EQ(bbo->ask_qty.raw, 30) << "existing level resized in place";
    EXPECT_EQ(book.bids().size(), 2U);
}

TEST(BookSync, DeletingAnAbsentLevelIsNotAnError) {
    auto book = make_book();
    const std::array bids{WireLevel{"100.0", "1.000"}};
    const std::array asks{WireLevel{"101.0", "1.000"}};
    ASSERT_EQ(book.apply(snapshot(bids, asks, 1)), SyncResult::Resynced);

    const std::array gone{WireLevel{"99.0", "0"}};
    EXPECT_EQ(book.apply(delta(gone, {}, 2)), SyncResult::Applied);
    EXPECT_EQ(book.bids().size(), 1U);
}

TEST(BookSync, UpdateIdGapDropsTheBook) {
    auto book = make_book();
    const std::array bids{WireLevel{"100.0", "1.000"}};
    const std::array asks{WireLevel{"101.0", "1.000"}};
    ASSERT_EQ(book.apply(snapshot(bids, asks, 10)), SyncResult::Resynced);

    // 11 was missed: the book we hold is fiction from here on.
    EXPECT_EQ(book.apply(delta(bids, asks, 12)), SyncResult::GapDetected);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.bbo(UnixNanos{1}, UnixNanos{2}).has_value())
        << "a desynchronised book must publish nothing at all";
    EXPECT_EQ(book.gaps(), 1U);

    // Recovery is a fresh snapshot, which the venue sends after resubscribe.
    EXPECT_EQ(book.apply(snapshot(bids, asks, 50)), SyncResult::Resynced);
    EXPECT_TRUE(book.synced());
}

TEST(BookSync, StaleAndDuplicateDeltasAreIgnored) {
    auto book = make_book();
    const std::array bids{WireLevel{"100.0", "1.000"}};
    const std::array asks{WireLevel{"101.0", "1.000"}};
    ASSERT_EQ(book.apply(snapshot(bids, asks, 10)), SyncResult::Resynced);
    ASSERT_EQ(book.apply(delta(bids, asks, 11)), SyncResult::Applied);

    EXPECT_EQ(book.apply(delta(bids, asks, 11)), SyncResult::Ignored);
    EXPECT_EQ(book.apply(delta(bids, asks, 5)), SyncResult::Ignored);
    EXPECT_EQ(book.gaps(), 0U) << "a replay is not a gap";
}

TEST(BookSync, DeltaBeforeSnapshotIsIgnoredNotApplied) {
    auto book = make_book();
    const std::array bids{WireLevel{"100.0", "1.000"}};
    EXPECT_EQ(book.apply(delta(bids, {}, 5)), SyncResult::Ignored);
    EXPECT_FALSE(book.synced());
}

TEST(BookSync, PriceOffTheTickGridIsRefused) {
    // A price that does not divide by the instrument's tick means bybit
    // changed a filter — rounding it would put orders at prices the venue
    // rejects, so the book is dropped instead.
    auto book = make_book();
    const std::array bad{WireLevel{"100.05", "1.000"}}; // tick is 0.1
    const std::array asks{WireLevel{"101.0", "1.000"}};
    EXPECT_EQ(book.apply(snapshot(bad, asks, 1)), SyncResult::BadLevel);
    EXPECT_FALSE(book.synced());
}

TEST(BookSync, InvalidateRequiresAFreshSnapshot) {
    auto book = make_book();
    const std::array bids{WireLevel{"100.0", "1.000"}};
    const std::array asks{WireLevel{"101.0", "1.000"}};
    ASSERT_EQ(book.apply(snapshot(bids, asks, 10)), SyncResult::Resynced);

    book.invalidate(); // what a reconnect does
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.apply(delta(bids, asks, 11)), SyncResult::Ignored);
    EXPECT_EQ(book.apply(snapshot(bids, asks, 99)), SyncResult::Resynced);
}

TEST(BookSync, OneSidedBookPublishesNoBbo) {
    auto book = make_book();
    const std::array bids{WireLevel{"100.0", "1.000"}};
    ASSERT_EQ(book.apply(snapshot(bids, {}, 1)), SyncResult::Resynced);
    EXPECT_FALSE(book.bbo(UnixNanos{1}, UnixNanos{2}).has_value());
}

TEST(BookSync, LevelsStaySortedAcrossManyUpdates) {
    auto book = make_book();
    const std::array bids{WireLevel{"100.0", "1.000"}};
    const std::array asks{WireLevel{"200.0", "1.000"}};
    ASSERT_EQ(book.apply(snapshot(bids, asks, 1)), SyncResult::Resynced);

    std::vector<std::string> prices;
    std::vector<WireLevel> inserts;
    for (int i = 1; i <= 20; ++i) {
        prices.push_back(std::to_string(100 + i) + ".0");
    }
    for (const auto& p : prices) {
        inserts.push_back(WireLevel{p, "1.000"});
    }
    EXPECT_EQ(book.apply(delta(inserts, {}, 2)), SyncResult::Applied);

    const auto levels = book.bids();
    ASSERT_EQ(levels.size(), 21U);
    for (std::size_t i = 1; i < levels.size(); ++i) {
        EXPECT_GT(levels[i - 1].price.raw, levels[i].price.raw) << "bids must stay descending";
    }
    EXPECT_EQ(levels.front().price.raw, 1200) << "120.0 / 0.1";
}

} // namespace
} // namespace raikiri::venues::bybit
