#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/nado/map.hpp>
#include <raikiri/venues/nado/nado_venue.hpp>
#include <raikiri/venues/nado/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace raikiri::venues::nado {
namespace {

std::string load(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << "missing fixture " << path.string();
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

struct Catalogue {
    exec::InstrumentTable table;
    InstrumentBase btc{};
};

Catalogue catalogue() {
    static thread_local SymbolBuffer storage;
    const auto body = load("rest_symbols.json");
    const auto rows = parse_symbols(body, storage);
    EXPECT_TRUE(rows.has_value());
    Catalogue out;
    for (const auto& row : *rows) {
        if (const auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(out.table.add(*instrument).has_value());
        }
    }
    out.btc = base_of(*out.table.find("BTC-PERP"));
    return out;
}

TEST(NadoVenue, RegistersAsAMarketDataOnlyPerpVenue) {
    EXPECT_EQ(to_string(VenueId::Nado), "nado");
    const auto caps = exec::caps_of(VenueId::Nado);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.ws_order_entry);
}

TEST(NadoCatalogue, LoadsTradablePerpsAndLeavesSpotAndHaltedOut) {
    static thread_local SymbolBuffer storage;
    const auto body = load("rest_symbols.json");
    const auto rows = parse_symbols(body, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 95U); // 82 perps and 13 spot on 2026-10-11

    const auto cat = catalogue();
    // 82 perps less the 5 not_tradable; post_only and soft_reduce_only still quote
    EXPECT_EQ(cat.table.size(), 77U);
    EXPECT_EQ(cat.btc.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(cat.btc.tick_size.raw, 1'000'000'000LL); // 1.0
    EXPECT_EQ(cat.btc.lot_size.raw, 50'000LL);         // 0.00005
    EXPECT_EQ(cat.btc.min_notional.raw, 100'000'000'000LL);
    EXPECT_EQ(cat.btc.maker_fee.raw, 100'000LL); // 1 bp
    EXPECT_EQ(cat.btc.taker_fee.raw, 350'000LL); // 3.5 bp
    const auto* perp = std::get_if<PerpInstrument>(cat.table.find("BTC-PERP"));
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(perp->funding_interval_s, 3600U);
}

TEST(NadoWire, ClassifiesEveryFrameItReads) {
    const auto kind = [](std::string_view name) {
        const auto frame = load(name);
        const auto event = parse_event(frame);
        EXPECT_TRUE(event.has_value()) << name;
        return event.has_value() ? event->kind : EventKind::Unknown;
    };
    EXPECT_EQ(kind("ws_bbo.json"), EventKind::Bbo);
    EXPECT_EQ(kind("ws_trade.json"), EventKind::Trade);
    EXPECT_EQ(kind("ws_depth_first.json"), EventKind::Depth);
    EXPECT_EQ(kind("ws_funding.json"), EventKind::Funding);
    EXPECT_EQ(kind("ws_subscribe_ack.json"), EventKind::Reply);

    const auto refused_frame = load("ws_subscribe_refused.json");
    const auto refused = parse_event(refused_frame);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->kind, EventKind::Reply);
    EXPECT_TRUE(refused->refused);
    const auto ack_frame = load("ws_subscribe_ack.json");
    EXPECT_FALSE(parse_event(ack_frame)->refused);
}

TEST(NadoMap, ReadsTheTopInX18) {
    const auto cat = catalogue();
    const auto frame = load("ws_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->product_id, 2U);
    EXPECT_EQ(wire->ts_ns, 1791713411109265183ULL);
    const auto bbo = map_bbo(*wire, cat.btc, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 83'013);
    EXPECT_EQ(bbo->ask.raw, 83'014);
    EXPECT_EQ(bbo->bid_qty.raw, 38'548); // 1.9274 / 0.00005
    EXPECT_EQ(bbo->ask_qty.raw, 34'538);
    EXPECT_EQ(bbo->ts_event.ns, 1791713411109265183ULL);
}

TEST(NadoMap, TakesTheTakerSideOfATrade) {
    const auto cat = catalogue();
    const auto frame = load("ws_trade.json");
    const auto wire = parse_trade(frame);
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_trade(*wire, cat.btc, UnixNanos{1});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 82'994);
    EXPECT_EQ(trade->qty.raw, 48); // 0.0024
    EXPECT_EQ(trade->aggressor, Side::Sell);
}

TEST(NadoMap, StatesTheDailyRateAsTheHourlyOneItSettles) {
    const auto cat = catalogue();
    const auto frame = load("ws_funding.json");
    const auto wire = parse_funding(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->rate_x18, "-145695784856258");
    const auto funding = map_funding(*wire, cat.btc, PriceTicks::from_raw(83'058), PriceTicks::undef(), UnixNanos{1});
    ASSERT_TRUE(funding.has_value());
    // -0.000145695784 a day, a 24th of it an hour
    EXPECT_EQ(funding->rate.raw, -145'695 / 24);
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->mark_price.raw, 83'058);
    // update_time 1791713418 is 10:50:18 utc; the next settlement is 11:00
    EXPECT_EQ(funding->next_funding_time.ns, 1791716400ULL * 1'000'000'000ULL);
}

TEST(NadoMap, AnX18BelowTheNinthDecimalIsRefusedWhereExactAndCutWhereNot) {
    EXPECT_EQ(x18_to_e9("1000000000000000000", true)->raw, 1'000'000'000LL);
    EXPECT_FALSE(x18_to_e9("1000000000000000001", true).has_value());
    EXPECT_EQ(x18_to_e9("1000000000000000001", false)->raw, 1'000'000'000LL);
    EXPECT_EQ(x18_to_e9("-145695784856258", false)->raw, -145'695LL);
    EXPECT_EQ(x18_to_e9("5", false)->raw, 0);
    EXPECT_FALSE(x18_to_e9("1e18", false).has_value());
    EXPECT_FALSE(x18_to_e9("", false).has_value());
}

TEST(NadoBook, AStaleSnapshotUnderAPushThatStartsLaterIsAGap) {
    const auto cat = catalogue();
    LevelBuffer levels;
    const auto body = load("rest_market_liquidity.json");
    const auto snapshot = parse_liquidity(body, levels);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->product_id, 2U);
    EXPECT_EQ(snapshot->max_ts, 1791713234393220381ULL);
    ASSERT_EQ(snapshot->bids.size(), 20U);

    Book book{cat.btc.id, cat.btc.tick_size, cat.btc.lot_size};
    ASSERT_EQ(book.resync(*snapshot), BookResult::Resynced);
    const auto top = book.snapshot(UnixNanos{1});
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bids[0].price.raw, 83'030);
    EXPECT_EQ(top->asks[0].price.raw, 83'031);

    // the capture's first push begins three minutes after this snapshot was taken
    LevelBuffer push_levels;
    const auto frame = load("ws_depth_first.json");
    const auto push = parse_depth(frame, push_levels);
    ASSERT_TRUE(push.has_value());
    EXPECT_EQ(book.apply(*push), BookResult::Gap);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.apply(*push), BookResult::Ignored);
}

TEST(NadoBook, PushesChainOnTheStampOfThePushBefore) {
    const auto cat = catalogue();
    LevelBuffer levels;
    const auto body = load("rest_market_liquidity.json");
    auto snapshot = parse_liquidity(body, levels);
    ASSERT_TRUE(snapshot.has_value());

    LevelBuffer first_levels;
    const auto first_frame = load("ws_depth_first.json");
    const auto first = parse_depth(first_frame, first_levels);
    ASSERT_TRUE(first.has_value());
    LevelBuffer next_levels;
    const auto next_frame = load("ws_depth_next.json");
    const auto next = parse_depth(next_frame, next_levels);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->last_max_ts, first->max_ts);

    // a snapshot stamped where the first push starts
    snapshot->max_ts = first->last_max_ts;
    Book book{cat.btc.id, cat.btc.tick_size, cat.btc.lot_size};
    ASSERT_EQ(book.resync(*snapshot), BookResult::Resynced);
    EXPECT_EQ(book.apply(*first), BookResult::Applied);
    EXPECT_EQ(book.apply(*next), BookResult::Applied);
    const auto after = book.snapshot(UnixNanos{1});
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->venue_seq, next->max_ts);
    // the same push again does not chain
    EXPECT_EQ(book.apply(*next), BookResult::Gap);
}

TEST(NadoContracts, OneCallCarriesMarkIndexAndTheDailyWindow) {
    const auto cat = catalogue();
    const auto body = load("rest_contracts.json");
    const auto storage = std::make_unique<ContractBuffer>();
    const auto rows = parse_contracts(body, *storage);
    ASSERT_TRUE(rows.has_value());
    const WireContract* btc = nullptr;
    for (const auto& row : *rows) {
        btc = row.product_id == 2 ? &row : btc;
    }
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(map_price(btc->mark_price, cat.btc)->raw, 83'058); // 83058.47490408621 floored
    EXPECT_EQ(map_price(btc->index_price, cat.btc)->raw, 83'074);
    const auto stats = map_ticker_stats(*btc, cat.btc, UnixNanos{1});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 643'027'300'000LL);
    EXPECT_EQ(stats->open_interest.raw, 304'068'700'000LL);
    EXPECT_EQ(stats->volume_24h_quote.raw / 1'000'000, 53'342'243LL);
}

TEST(NadoWire, ReadsTheBareMillisecondClock) {
    const auto body = load("rest_time.txt");
    const auto ms = parse_server_time_ms(body);
    ASSERT_TRUE(ms.has_value());
    EXPECT_EQ(*ms, 1791714374438ULL);
    EXPECT_FALSE(parse_server_time_ms("{\"x\":1}").has_value());
}

} // namespace
} // namespace raikiri::venues::nado
