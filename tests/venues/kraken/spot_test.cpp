#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/catalogue.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/spot.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::kraken {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::vector<Instrument> spot_catalogue() {
    static const auto storage = std::make_unique<SpotPairBuffer>();
    static const auto body = fixture("spot_rest_asset_pairs.json");
    const auto rows = parse_asset_pairs(body, *storage);
    EXPECT_TRUE(rows.has_value());
    std::vector<Instrument> out;
    for (const auto& row : *rows) {
        if (auto instrument = map_spot_pair(row); instrument.has_value()) {
            out.push_back(*instrument);
        }
    }
    return out;
}

exec::InstrumentTable spot_table() {
    exec::InstrumentTable table;
    for (const auto& instrument : spot_catalogue()) {
        EXPECT_TRUE(table.add(instrument).has_value());
    }
    return table;
}

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

TEST(KrakenSpot, CatalogueKeepsOnlineRowsThatFitFixedPoint) {
    const auto body = fixture("spot_rest_asset_pairs.json");
    SpotPairBuffer storage;
    const auto rows = parse_asset_pairs(body, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 6U);

    // CAT/USD prices to ten decimals and ACA/EUR is cancel_only
    const auto loaded = spot_catalogue();
    ASSERT_EQ(loaded.size(), 4U);
    std::vector<std::string_view> names;
    for (const auto& instrument : loaded) {
        EXPECT_TRUE(std::holds_alternative<SpotInstrument>(instrument));
        names.push_back(base_of(instrument).exchange_symbol.view());
    }
    EXPECT_EQ(names, (std::vector<std::string_view>{"BTC/USD", "DOGE/USD", "PEPE/USD", "ETH/BTC"}));

    const auto table = spot_table();
    const auto* btc = table.find("BTC/USD");
    ASSERT_NE(btc, nullptr);
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.id.market(), MarketType::Spot);
    EXPECT_EQ(base.tick_size.raw, 100'000'000LL); // pair_decimals 1
    EXPECT_EQ(base.lot_size.raw, 10LL);           // lot_decimals 8
    EXPECT_EQ(base.min_notional.raw, 500'000'000LL);
}

TEST(KrakenSpot, AnErrorEnvelopeRefusesTheCatalogue) {
    SpotPairBuffer storage;
    EXPECT_FALSE(parse_asset_pairs(R"({"error":["EGeneral:Invalid arguments"]})", storage).has_value());
    EXPECT_FALSE(parse_asset_pairs("not json", storage).has_value());
}

TEST(KrakenSpot, WsnameMapsToTheV2SocketName) {
    const auto name = [](std::string_view wsname) {
        const auto symbol = v2_symbol(wsname);
        return symbol.has_value() ? std::string{symbol->view()} : std::string{"-"};
    };
    EXPECT_EQ(name("XBT/USD"), "BTC/USD");
    EXPECT_EQ(name("ETH/XBT"), "ETH/BTC");
    EXPECT_EQ(name("XDG/XBT"), "DOGE/BTC");
    // only a whole asset is renamed
    EXPECT_EQ(name("AIXBT/USD"), "AIXBT/USD");
    EXPECT_EQ(name("XDC/USD"), "XDC/USD");
    EXPECT_EQ(name("XBTUSD"), "-");
    EXPECT_EQ(name("A/B/C"), "-");
}

TEST(KrakenSpot, SpotAndPerpShareTheLocalSymbol) {
    const auto spot = spot_table();
    InstrumentBuffer storage;
    const auto body = fixture("rest_instruments.json");
    const auto rows = parse_instruments(body, storage);
    ASSERT_TRUE(rows.has_value());
    exec::InstrumentTable perp;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            ASSERT_TRUE(perp.add(*instrument).has_value());
        }
    }
    const auto* pf = perp.find("PF_XBTUSD");
    const auto* btc = spot.find("BTC/USD");
    ASSERT_NE(pf, nullptr);
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(base_of(*pf).local_symbol.view(), "BTC/USD");
    EXPECT_EQ(base_of(*btc).local_symbol.view(), "BTC/USD");
    EXPECT_EQ(base_of(*spot.find("DOGE/USD")).local_symbol.view(), "DOGE/USD");
}

TEST(KrakenSpot, EventsResolveTheirChannel) {
    for (const auto& [file, channel] : {std::pair{"spot_ws_ticker_snapshot.json", SpotChannel::Ticker},
                                        std::pair{"spot_ws_trade.json", SpotChannel::Trade},
                                        std::pair{"spot_ws_book_snapshot.json", SpotChannel::Book},
                                        std::pair{"spot_ws_book_update.json", SpotChannel::Book},
                                        std::pair{"spot_ws_subscribed.json", SpotChannel::Ack},
                                        std::pair{"spot_ws_refused.json", SpotChannel::Refused},
                                        std::pair{"spot_ws_status.json", SpotChannel::Other},
                                        std::pair{"spot_ws_heartbeat.json", SpotChannel::Other}}) {
        const auto event = parse_spot_event(fixture(file));
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->channel, channel) << file;
    }
    EXPECT_TRUE(parse_spot_event(fixture("spot_ws_book_snapshot.json"))->snapshot);
    EXPECT_FALSE(parse_spot_event(fixture("spot_ws_book_update.json"))->snapshot);
}

TEST(KrakenSpot, TickerYieldsTouchAndTurnoverOnExchangeTime) {
    const auto table = spot_table();
    const auto frame = fixture("spot_ws_ticker_snapshot.json");
    const auto wire = parse_spot_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const UnixNanos received{1};
    const auto bbo = map_spot_bbo(*wire, table, received);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 851864);
    EXPECT_EQ(bbo->ask.raw, 851865);
    EXPECT_EQ(bbo->bid_qty.raw, 160431568);
    EXPECT_EQ(bbo->ask_qty.raw, 62751088);
    EXPECT_EQ(bbo->ts_event.ns, 1'791'123'308'875'428'000ULL); // 2026-10-04T14:15:08.875428Z
    EXPECT_EQ(bbo->ts_init.ns, received.ns);

    const auto stats = map_spot_ticker_stats(*wire, table, received);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 767'531'169'840LL);
    EXPECT_EQ(stats->volume_24h_quote.raw, 65'191'411'207'115LL); // volume times vwap
}

TEST(KrakenSpot, AMemeCoinsTurnoverSurvivesABaseVolumePastFixedPoint) {
    const auto table = spot_table();
    const auto frame = fixture("spot_ws_ticker_pepe.json");
    const auto wire = parse_spot_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_spot_ticker_stats(*wire, table, UnixNanos{1});
    ASSERT_TRUE(stats.has_value());
    // 153 billion PEPE does not fit e9, the dollar figure does
    EXPECT_EQ(stats->volume_24h_base.raw, 0);
    EXPECT_EQ(stats->volume_24h_quote.raw, 656'586'988'405LL);
    const auto bbo = map_spot_bbo(*wire, table, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 4271);
}

TEST(KrakenSpot, TradeKeepsAggressorAndExchangeTime) {
    const auto table = spot_table();
    SpotTradeBuffer storage;
    const auto frame = fixture("spot_ws_trade.json");
    const auto wire = parse_spot_trades(frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    EXPECT_EQ(wire->front().trade_id, 110127536U);
    const auto trade = map_spot_trade(wire->front(), table, UnixNanos{1});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price.raw, 851864);
    EXPECT_EQ(trade->qty.raw, 85810);
    EXPECT_EQ(trade->ts_event.ns, 1'791'123'309'839'155'000ULL);
}

SpotBook book_for(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto& base = base_of(*table.find(symbol));
    return SpotBook{base.id, base.tick_size, base.lot_size, k_spot_book_depth};
}

TEST(KrakenSpotBook, LiveFramesChainUnderTheirChecksums) {
    const auto table = spot_table();
    auto book = book_for(table, "PEPE/USD");
    LevelBuffer levels;
    const auto snapshot_text = fixture("spot_ws_book_snapshot.json");
    const auto snapshot = parse_spot_book(snapshot_text, levels);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->bids.size(), 10U);
    EXPECT_EQ(book.apply(*snapshot), SpotBookResult::Resynced);
    for (const auto* file : {"spot_ws_book_update.json", "spot_ws_book_update_next.json"}) {
        const auto text = fixture(file);
        const auto update = parse_spot_book(text, levels);
        ASSERT_TRUE(update.has_value()) << file;
        EXPECT_EQ(book.apply(*update), SpotBookResult::Applied) << file;
    }
    EXPECT_EQ(book.bids().size(), 10U);
    EXPECT_EQ(book.mismatches(), 0U);
    const auto depth = book.snapshot(UnixNanos{2}, UnixNanos{3});
    ASSERT_TRUE(depth.has_value());
}

TEST(KrakenSpotBook, ChecksumMatchesTheVenuesOnABitcoinSnapshot) {
    const auto table = spot_table();
    auto book = book_for(table, "BTC/USD");
    LevelBuffer levels;
    const auto snapshot_text = fixture("spot_ws_book_snapshot_btc.json");
    const auto snapshot = parse_spot_book(snapshot_text, levels);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->checksum, 420097310U);
    ASSERT_EQ(book.apply(*snapshot), SpotBookResult::Resynced);
    EXPECT_EQ(spot_book_checksum(book.bids(), book.asks()), 420097310U);
}

TEST(KrakenSpotBook, AMismatchDropsTheBookUntilTheNextSnapshot) {
    const auto table = spot_table();
    auto book = book_for(table, "PEPE/USD");
    LevelBuffer levels;
    const auto snapshot_text = fixture("spot_ws_book_snapshot.json");
    ASSERT_EQ(book.apply(*parse_spot_book(snapshot_text, levels)), SpotBookResult::Resynced);

    // the second update applied without the first leaves a book kraken never had
    const auto skipped_text = fixture("spot_ws_book_update_next.json");
    const auto skipped = parse_spot_book(skipped_text, levels);
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(book.apply(*skipped), SpotBookResult::ChecksumMismatch);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.mismatches(), 1U);
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());

    const auto later_text = fixture("spot_ws_book_update.json");
    const auto later = parse_spot_book(later_text, levels);
    EXPECT_EQ(book.apply(*later), SpotBookResult::Ignored);
    EXPECT_EQ(book.apply(*parse_spot_book(snapshot_text, levels)), SpotBookResult::Resynced);
}

TEST(KrakenSpotBook, ChecksumReadsAsksThenBidsAsDigits) {
    // asks 0.5 x 1.00 and 0.6 x 2.00, bid 0.4 x 3.00, at one price and two size decimals
    const std::array<book::Level, 2> asks{
        {{PriceTicks::from_raw(5), QtyLots::from_raw(100)}, {PriceTicks::from_raw(6), QtyLots::from_raw(200)}}};
    const std::array<book::Level, 1> bids{{{PriceTicks::from_raw(4), QtyLots::from_raw(300)}}};
    // crc32("5100" "6200" "4300")
    EXPECT_EQ(spot_book_checksum(bids, asks), 0x21873287U);
}

TEST(KrakenSpot, FramesCarryTheChannelParameters) {
    const std::array<std::string_view, 2> symbols{"BTC/USD", "DOGE/USD"};
    EXPECT_EQ(
        spot_frame("subscribe", "ticker", symbols),
        R"({"method":"subscribe","params":{"channel":"ticker","symbol":["BTC/USD","DOGE/USD"],"event_trigger":"bbo"}})");
    EXPECT_EQ(spot_frame("unsubscribe", "book", std::span{symbols}.first(1)),
              R"({"method":"unsubscribe","params":{"channel":"book","symbol":["BTC/USD"],"depth":10}})");
    EXPECT_EQ(spot_channel_for(exec::SubKind::Bbo), "ticker");
    EXPECT_EQ(spot_channel_for(exec::SubKind::Trades), "trade");
    EXPECT_EQ(spot_channel_for(exec::SubKind::Depth), "book");
    EXPECT_TRUE(spot_channel_for(exec::SubKind::Funding).empty());
}

TEST(KrakenSpot, TimestampsKeepMicroseconds) {
    EXPECT_EQ(*parse_iso8601_ns("2026-10-04T14:15:08.875428Z"), 1'791'123'308'875'428'000ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-10-04T14:15:08.875428Z"), 1'791'123'308'875ULL);
}

TEST(KrakenSpot, OrdersAreRefusedAndStartNeedsNoCredentials) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Kraken, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_TRUE(adapter->caps.markets.spot);
    EXPECT_TRUE(adapter->execution->start().has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 7);
    cmd.instrument = InstrumentId::make(VenueId::Kraken, MarketType::Spot, SymbolId{1});
    adapter->execution->submit(cmd);
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

} // namespace
} // namespace raikiri::venues::kraken
