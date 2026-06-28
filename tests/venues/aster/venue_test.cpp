// Aster parser tests, against bytes captured live from the venue on
// 2026-09-20. See fixtures/README.md for provenance.
//
// WHY A WHOLE TEST FILE FOR A WIRE-COMPATIBLE API: this venue serves the same
// paths and field names as a much larger one, and the parsers here were forked
// from that adapter. That makes the tests MORE necessary, not less — a fork
// that silently drifts from its origin is exactly what nobody notices, and the
// weight table below already differs.

#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/market_data.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::aster {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Every parser takes the payload, not the {"stream":..,"data":..} wrapper.
std::string data_of(std::string_view name) {
    const auto frame = fixture(name);
    const auto inner = unwrap(frame);
    EXPECT_TRUE(inner.has_value());
    return std::string{*inner};
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.base_asset = "BTC";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.min_notional = "5";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

// --- the catalogue -------------------------------------------------------

TEST(AsterWire, ExchangeInfoParsesTheThreeShapesTheVenueServes) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    // The fixture keeps one plain perp, one non-USDT quote and one not trading.
    ASSERT_EQ(rows->size(), 3U);

    bool saw_btc = false;
    for (const auto& row : *rows) {
        if (row.symbol == "BTCUSDT") {
            saw_btc = true;
            EXPECT_EQ(row.contract_type, "PERPETUAL");
            EXPECT_EQ(row.quote_asset, "USDT");
        }
    }
    EXPECT_TRUE(saw_btc);
}

TEST(AsterMap, ANonUsdtQuoteSurvivesTheMapper) {
    // 11 of 602 rows are quoted in USD1 and 2 in "U" (read live 2026-09-20).
    // A mapper that assumed USDT would drop them, and the board would then be
    // missing the pairs most likely to carry a wide funding rate.
    WireInstrument wire{};
    wire.symbol = "BTCUSD1";
    wire.base_asset = "BTC";
    wire.quote_asset = "USD1";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
}

TEST(AsterMap, ARowThatIsNotTradingIsRefused) {
    WireInstrument wire{};
    wire.symbol = "PENDINGUSDT";
    wire.base_asset = "PENDING";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "PENDING_TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

// --- market data ---------------------------------------------------------

TEST(AsterWire, EventDispatchResolvesKindAndSymbol) {
    for (const auto& [file, kind] : {std::pair{"ws_book_ticker.json", EventKind::BookTicker},
                                     std::pair{"ws_agg_trade.json", EventKind::Trade},
                                     std::pair{"ws_mark_price.json", EventKind::MarkPrice},
                                     std::pair{"ws_depth_update.json", EventKind::DepthUpdate}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        EXPECT_EQ(event->symbol, "BTCUSDT") << file;
    }
}

TEST(AsterMap, BookTickerBecomesBbo) {
    const auto table = table_with_btc();
    const auto payload = data_of("ws_book_ticker.json");
    const auto wire = parse_book_ticker(payload);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_GT(bbo->bid.raw, 0);
    EXPECT_GT(bbo->ask.raw, bbo->bid.raw);
    EXPECT_NE(bbo->ts_event.ns, 0ULL);
}

TEST(AsterMap, MarkPriceCarriesTheFundingRate) {
    const auto table = table_with_btc();
    const auto payload = data_of("ws_mark_price.json");
    const auto wire = parse_mark_price(payload);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // A contract quoting no rate must not be published as one funding at zero,
    // so the mapper is allowed to refuse; this fixture does quote one.
    EXPECT_NE(funding->next_funding_time.ns, 0ULL);
}

TEST(AsterBook, ConsecutiveDepthFramesChainByPrevUpdateId) {
    // THE INVARIANT THIS VENUE INHERITS: frame N's `u` is frame N+1's `pu`.
    // Captured live as two consecutive frames; if the venue ever stops
    // chaining, the book must resynchronise rather than apply into a hole.
    const auto first = data_of("ws_depth_update.json");
    const auto second = data_of("ws_depth_update_next.json");
    LevelBuffer buf_a;
    LevelBuffer buf_b;
    const auto a = parse_depth_update(first, buf_a);
    const auto b = parse_depth_update(second, buf_b);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->last_id, b->prev_id);
    EXPECT_LE(a->first_id, a->last_id);
}

// --- what a call costs ---------------------------------------------------

TEST(AsterEndpoints, TestnetHasNowhereToPointSoBothAccessorsAnswerLive) {
    // Neither testnet host resolves (checked 2026-09-20). Naming a dead one
    // would fail at connect time, far from the config that asked for it.
    EXPECT_EQ(ep::rest_host(true), ep::k_rest_live);
    EXPECT_EQ(ep::stream_host(true), ep::k_stream_live);
}

} // namespace
} // namespace raikiri::venues::aster
