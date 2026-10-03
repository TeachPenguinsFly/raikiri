#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/venues/derive/derive_venue.hpp>
#include <raikiri/venues/derive/map.hpp>
#include <raikiri/venues/derive/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace raikiri::venues::derive {
namespace {

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// the captures were taken at 2026-10-03 13:59:56 utc
constexpr UnixNanos k_now{1'791'040'796'000'000'000ULL};
constexpr UnixNanos k_ingest{1'791'040'797'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

std::vector<Instrument> load(exec::InstrumentTable& table, MarketType market, const std::string& body) {
    auto storage = std::make_unique<InstrumentBuffer>();
    const auto page = parse_instruments(body, *storage);
    EXPECT_TRUE(page.has_value());
    std::vector<Instrument> loaded;
    if (!page.has_value()) {
        return loaded;
    }
    for (const auto& row : page->rows) {
        auto instrument = map_instrument(row, market, k_now);
        if (!instrument.has_value()) {
            continue;
        }
        classify(VenueId::Derive, *instrument);
        if (const auto id = table.add(*instrument); id.has_value()) {
            loaded.push_back(*table.find(*id));
        }
    }
    return loaded;
}

const InstrumentBase& base_named(const exec::InstrumentTable& table, std::string_view name) {
    const auto* instrument = table.find(name);
    EXPECT_NE(instrument, nullptr) << name;
    return base_of(*instrument);
}

TEST(DeriveVenue, RegistersBuildsAndServesOptionsAndPerpetualsOnly) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    for (const auto market : {MarketType::Option, MarketType::Perp}) {
        const auto adapter = exec::make_venue({.venue = VenueId::Derive, .market = market}, sink, clock);
        ASSERT_TRUE(adapter.has_value()) << to_string(market);
        EXPECT_EQ(adapter->caps.venue, VenueId::Derive);
        EXPECT_TRUE(adapter->credential_errors.empty());
    }
    for (const auto market : {MarketType::Spot, MarketType::Future}) {
        const auto refused = exec::make_venue({.venue = VenueId::Derive, .market = market}, sink, clock);
        ASSERT_FALSE(refused.has_value()) << to_string(market);
        EXPECT_EQ(refused.error(), exec::MakeError::UnsupportedMarket);
    }
    const auto caps = exec::caps_of(VenueId::Derive);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.order_pull.option);
}

TEST(DeriveVenue, EveryOrderIsRefusedAsNotImplemented) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Derive, .market = MarketType::Option}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    ASSERT_TRUE(adapter->execution->start().has_value());
    EXPECT_FALSE(adapter->execution->connected());

    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Derive, MarketType::Option, SymbolId{1});
    adapter->execution->submit(cmd);
    adapter->execution->cancel(cmd);
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);

    const auto pulled = adapter->execution->open_order_reports(MarketType::Option);
    ASSERT_FALSE(pulled.has_value());
    EXPECT_EQ(pulled.error().message.view(), "derive: order entry not implemented");
}

TEST(DeriveVenue, AReferenceThatDoesNotResolveStopsStartUp) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue({.venue = VenueId::Derive,
                                           .market = MarketType::Perp,
                                           .api_key_env = "env:RAIKIRI_DERIVE_TEST_UNSET_KEY",
                                           .api_secret_env = "env:RAIKIRI_DERIVE_TEST_UNSET_SECRET"},
                                          sink,
                                          clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_FALSE(adapter->credential_errors.empty());
}

TEST(DeriveCatalogue, OnlyActiveOptionsLoadWithTheirStrikeOnThePremiumTick) {
    exec::InstrumentTable table;
    const auto loaded = load(table, MarketType::Option, fixture("rest_get_all_instruments_option.json"));
    // ETH-20271231-6000-P is listed ahead of its activation
    ASSERT_EQ(loaded.size(), 3U);
    EXPECT_EQ(table.find("ETH-20271231-6000-P"), nullptr);

    const auto* call = table.find("ETH-20261009-2675-C");
    ASSERT_NE(call, nullptr);
    const auto& option = std::get<OptionInstrument>(*call);
    EXPECT_EQ(option.put_call, PutCall::Call);
    EXPECT_EQ(option.expiry.ns, 1'791'532'800ULL * 1'000'000'000ULL); // 2026-10-09 08:00 utc
    EXPECT_EQ(option.base.tick_size, e9("0.1"));
    EXPECT_EQ(option.base.lot_size, e9("0.01"));
    EXPECT_EQ(option.strike.raw, 26'750);
    EXPECT_EQ(option.base.ticks_to_price(option.strike), e9("2675"));
    EXPECT_EQ(option.base.kind, ContractKind::Linear);
    EXPECT_EQ(option.base.contract_value, e9("1"));
    EXPECT_EQ(option.premium, OptionUnit::Quote);
    EXPECT_EQ(option.greeks, OptionUnit::Quote);
    EXPECT_EQ(option.base.underlying.view(), "ETH");
    EXPECT_EQ(option.base.maker_fee, e9("0.0001"));

    // the underscore is the strike's decimal point in the name; the catalogue states 4.5
    const auto* xrp = table.find("XRP-20261225-4_5-P");
    ASSERT_NE(xrp, nullptr);
    const auto& put = std::get<OptionInstrument>(*xrp);
    EXPECT_EQ(put.put_call, PutCall::Put);
    EXPECT_EQ(put.base.ticks_to_price(put.strike), e9("4.5"));
    EXPECT_EQ(put.base.underlying.view(), "XRP");
}

TEST(DeriveCatalogue, ARowIsRefusedBeforeItsActivationAndOutsideItsMarket) {
    auto storage = std::make_unique<InstrumentBuffer>();
    const auto body = fixture("rest_get_all_instruments_option.json");
    const auto page = parse_instruments(body, *storage);
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->num_pages, 5U);
    const auto& row = page->rows.front();
    EXPECT_TRUE(map_instrument(row, MarketType::Option, k_now).has_value());
    EXPECT_FALSE(map_instrument(row, MarketType::Option, UnixNanos{(row.activation_s - 1) * 1'000'000'000ULL}));
    EXPECT_FALSE(map_instrument(row, MarketType::Perp, k_now).has_value());
}

TEST(DeriveCatalogue, PerpetualsPairByBaseAndQuote) {
    exec::InstrumentTable table;
    const auto loaded = load(table, MarketType::Perp, fixture("rest_get_all_instruments_perp.json"));
    ASSERT_EQ(loaded.size(), 3U);
    const auto* eth = table.find("ETH-PERP");
    ASSERT_NE(eth, nullptr);
    const auto& perp = std::get<PerpInstrument>(*eth);
    EXPECT_EQ(perp.base.local_symbol.view(), "ETH/USD");
    EXPECT_EQ(perp.base.tick_size, e9("0.01"));
    EXPECT_EQ(perp.base.lot_size, e9("0.001"));
    EXPECT_EQ(perp.funding_interval_s, 3600U);
    EXPECT_EQ(perp.base.underlying.view(), "ETH");
}

TEST(DeriveCatalogue, AnErrorBodyIsReadAsTheVenuesCode) {
    const auto body = fixture("rest_error_bad_instrument_type.json");
    const auto error = parse_rpc_error(body);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, -32602);
    EXPECT_EQ(error->message, "Invalid params");
    EXPECT_FALSE(parse_rpc_error(fixture("rest_get_all_instruments_perp.json")).has_value());
}

TEST(DeriveWire, TheChannelNamesTheStreamAndTheInstrument) {
    const auto book_frame = fixture("ws_orderbook_option.json");
    const auto book = parse_head(book_frame);
    EXPECT_EQ(book.channel, Channel::Book);
    EXPECT_EQ(instrument_of(book.name, 2), "ETH-20261009-2675-C");
    const auto ticker_frame = fixture("ws_ticker_slim_perp.json");
    const auto ticker = parse_head(ticker_frame);
    EXPECT_EQ(ticker.channel, Channel::Ticker);
    EXPECT_EQ(instrument_of(ticker.name, 1), "ETH-PERP");
    EXPECT_EQ(parse_head(fixture("ws_trades_perp.json")).channel, Channel::Trades);
    EXPECT_EQ(parse_head(fixture("ws_subscribe_ack.json")).channel, Channel::Response);
    EXPECT_EQ(parse_head("pong").channel, Channel::Unknown);
    EXPECT_EQ(instrument_of("trades.XRP-20261225-4_5-P", 0), "XRP-20261225-4_5-P");
}

TEST(DeriveWire, RefusedChannelsAreCountedFromTheStatusMap) {
    const auto clean = parse_subscribe_ack(fixture("ws_subscribe_ack.json"));
    ASSERT_TRUE(clean.has_value());
    EXPECT_EQ(clean->accepted, 9U);
    EXPECT_EQ(clean->refused, 0U);
    const auto refused = parse_subscribe_ack(fixture("ws_subscribe_ack_refused.json"));
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->accepted, 6U);
    EXPECT_EQ(refused->refused, 2U);
}

TEST(DeriveMap, BookSnapshotYieldsTheTopAndTheLadder) {
    exec::InstrumentTable table;
    (void)load(table, MarketType::Option, fixture("rest_get_all_instruments_option.json"));
    LevelBuffer levels;
    const auto frame = fixture("ws_orderbook_option.json");
    const auto wire = parse_book(frame, levels);
    ASSERT_TRUE(wire.has_value());
    const auto& base = base_named(table, wire->instrument);

    const auto bbo = map_bbo(*wire, base, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(base.ticks_to_price(bbo->bid), e9("51.7"));
    EXPECT_EQ(base.ticks_to_price(bbo->ask), e9("55.2"));
    EXPECT_EQ(base.lots_to_qty(bbo->bid_qty), e9("0.74"));
    EXPECT_EQ(base.lots_to_qty(bbo->ask_qty), e9("66.24"));
    EXPECT_EQ(bbo->ts_event.ns, 1'791'040'796'764'000'000ULL);

    const auto book = map_book(*wire, base, k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 8U);
    EXPECT_EQ(book->ask_count, 5U);
    EXPECT_EQ(book->venue_seq, 278'098U);
    EXPECT_LT(book->bids[1].price, book->bids[0].price);
    EXPECT_LT(book->asks[0].price, book->asks[1].price);
}

TEST(DeriveMap, AOneSidedBookHasNoTop) {
    exec::InstrumentTable table;
    (void)load(table, MarketType::Option, fixture("rest_get_all_instruments_option.json"));
    LevelBuffer levels;
    const std::string frame =
        R"({"method":"subscription","params":{"channel":"orderbook.ETH-20261009-2675-P.1.1","data":{"timestamp":1791040796764,"instrument_name":"ETH-20261009-2675-P","publish_id":7,"bids":[],"asks":[["12.3","4"]]}}})";
    const auto wire = parse_book(frame, levels);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_bbo(*wire, base_named(table, "ETH-20261009-2675-P"), k_ingest).has_value());
}

TEST(DeriveMap, TheOptionTickerCarriesTheVenuesGreeksInTheSharedConvention) {
    exec::InstrumentTable table;
    (void)load(table, MarketType::Option, fixture("rest_get_all_instruments_option.json"));
    const auto frame = fixture("ws_ticker_slim_option.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto& base = base_named(table, "ETH-20261009-2675-C");

    EXPECT_FALSE(map_funding(*wire, base, k_ingest).has_value());
    const auto greeks = map_greeks(*wire, base, k_ingest);
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument, base.id);
    EXPECT_EQ(greeks->delta, e9("0.53074"));
    EXPECT_EQ(greeks->gamma, e9("0.00353264"));
    // already per vol point and per day: black-76 on this frame gives 1.331 and -3.93
    EXPECT_EQ(greeks->vega, e9("1.3317"));
    EXPECT_EQ(greeks->theta, e9("-3.9311"));
    EXPECT_EQ(greeks->rho, e9("0.75002"));
    // a fraction on the wire, unlike deribit's percent
    EXPECT_EQ(greeks->mark_iv, e9("0.33619"));
    EXPECT_EQ(greeks->bid_iv, e9("0.36341"));
    EXPECT_EQ(greeks->ask_iv, e9("0.38968"));
    EXPECT_EQ(greeks->underlying_price, e9("2681.3")); // the forward
    EXPECT_EQ(greeks->mark_price, e9("48"));           // usdc premium
    EXPECT_EQ(greeks->index_price, e9("2680.4"));
    EXPECT_EQ(greeks->ts_event.ns, 1'791'040'796'764'000'000ULL);
    EXPECT_EQ(greeks->ts_init, k_ingest);

    const auto stats = map_ticker_stats(*wire, base, MarketType::Option, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("1"));
    EXPECT_EQ(stats->volume_24h_quote.raw, 51'000'000); // premium, not notional
}

TEST(DeriveMap, ThePerpetualTickerCarriesAnHourlyContinuousRateAndNoGreeks) {
    exec::InstrumentTable table;
    (void)load(table, MarketType::Perp, fixture("rest_get_all_instruments_perp.json"));
    const auto frame = fixture("ws_ticker_slim_perp.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto& base = base_named(table, "ETH-PERP");

    EXPECT_FALSE(map_greeks(*wire, base, k_ingest).has_value());
    const auto funding = map_funding(*wire, base, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.0000125"));
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Continuous);
    EXPECT_EQ(base.ticks_to_price(funding->mark_price), e9("2679.53"));
    EXPECT_EQ(funding->ts_event.ns, 1'791'040'796'613'000'000ULL);

    const auto stats = map_ticker_stats(*wire, base, MarketType::Perp, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("3596.888"));
    EXPECT_EQ(stats->volume_24h_quote.raw, 9'621'026'611'000);
}

TEST(DeriveMap, TradeDirectionIsTheAggressor) {
    exec::InstrumentTable table;
    (void)load(table, MarketType::Perp, fixture("rest_get_all_instruments_perp.json"));
    TradeBuffer storage;
    const auto frame = fixture("ws_trades_perp.json");
    const auto wire = parse_trades(frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    const auto& base = base_named(table, (*wire)[0].instrument);
    const auto trade = map_trade((*wire)[0], base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(base.ticks_to_price(trade->price), e9("88.979"));
    EXPECT_EQ(base.lots_to_qty(trade->qty), e9("20"));
    EXPECT_EQ(trade->ts_event.ns, 1'791'040'852'431'000'000ULL);
}

TEST(DeriveChain, TheCatalogueFormsACallPutPairPerStrike) {
    exec::InstrumentTable table;
    const auto loaded = load(table, MarketType::Option, fixture("rest_get_all_instruments_option.json"));
    const auto chain = options::OptionChain::build(loaded);
    EXPECT_EQ(chain.size(), 3U);
    const auto* slice = chain.expiry("ETH", UnixNanos{1'791'532'800ULL * 1'000'000'000ULL});
    ASSERT_NE(slice, nullptr);
    ASSERT_EQ(slice->rows.size(), 1U);
    EXPECT_EQ(slice->rows[0].strike, e9("2675"));
    ASSERT_TRUE(slice->rows[0].call.has_value());
    ASSERT_TRUE(slice->rows[0].put.has_value());
    EXPECT_EQ(*slice->rows[0].call, base_named(table, "ETH-20261009-2675-C").id);
    EXPECT_EQ(*slice->rows[0].put, base_named(table, "ETH-20261009-2675-P").id);
}

} // namespace
} // namespace raikiri::venues::derive
