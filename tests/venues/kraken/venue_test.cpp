#include <raikiri/domain/asset_class.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/rest.hpp>
#include <raikiri/venues/kraken/sign.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
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

// built the way the adapter builds it: parse, map, intern
exec::InstrumentTable catalogue() {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_instruments.json");
    const auto wire = parse_instruments(body, storage);
    EXPECT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *wire) {
        if (auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
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

TEST(KrakenVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue({.venue = VenueId::Kraken, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Kraken);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(KrakenVenue, OrderEntryRefusesEveryMarketThisAdapterDoesNotServe) {
    // spot is refused by name, not reported as an unknown instrument
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Kraken, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    for (const auto market : {MarketType::Spot, MarketType::Future, MarketType::Option}) {
        const auto orders = adapter->execution->open_order_reports(market);
        ASSERT_FALSE(orders.has_value()) << to_string(market);
        EXPECT_EQ(orders.error().cls, exec::ErrorClass::Terminal);
        EXPECT_FALSE(adapter->execution->fill_reports(market, UnixNanos{0}).has_value());
        EXPECT_FALSE(adapter->execution->position_reports(market).has_value());
    }

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.instrument = InstrumentId::make(VenueId::Kraken, MarketType::Spot, SymbolId{0});
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
}

TEST(KrakenVenue, PullsWithoutCredentialsAnswerAuthFailureNotTerminal) {
    // a missing key is fixable, so it is not terminal
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Kraken, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto started = adapter->execution->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().cls, exec::ErrorClass::AuthFailure);

    const auto orders = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().cls, exec::ErrorClass::AuthFailure);
    // a pull the adapter cannot perform must say so; an empty vector would read as flat
    const auto positions = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(positions.has_value());
    EXPECT_EQ(positions.error().cls, exec::ErrorClass::AuthFailure);
}

TEST(KrakenVenue, TestnetIsRefusedRatherThanRedirectedToProduction) {
    // kraken has no demo host, so --testnet refuses to start
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Kraken, .market = MarketType::Perp, .testnet = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_FALSE(adapter->data->start().has_value());
}

TEST(KrakenWire, EventDispatchResolvesKindAndProduct) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker_pf.json", EventKind::Ticker},
                                     std::pair{"ws_trade.json", EventKind::Trade},
                                     std::pair{"ws_trade_snapshot.json", EventKind::TradeSnapshot},
                                     std::pair{"ws_book_snapshot.json", EventKind::Book},
                                     std::pair{"ws_book_delta.json", EventKind::Book}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        EXPECT_EQ(event->product_id, "PF_XBTUSD") << file;
    }
}

TEST(KrakenWire, ControlFramesAreNotMistakenForData) {
    // a refusal and an ack differ by one field value
    const auto ack = fixture("ws_subscribed.json");
    auto event = parse_event(ack);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Subscribed);

    const auto info = fixture("ws_info.json");
    event = parse_event(info);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Info);

    // the refusal event is alert with a message and no code
    const auto bad = fixture("ws_alert_bad_product.json");
    event = parse_event(bad);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Alert);
    const auto alert = parse_stream_alert(bad);
    ASSERT_TRUE(alert.has_value());
    EXPECT_NE(alert->message.find("invalid product"), std::string_view::npos);

    const auto bad_feed = fixture("ws_alert_bad_feed.json");
    EXPECT_EQ(parse_event(bad_feed)->kind, EventKind::Alert);
    EXPECT_TRUE(parse_stream_alert(bad_feed).has_value());
}

TEST(KrakenWire, MalformedFrameIsAnErrorNotAPartlyFilledStruct) {
    EXPECT_FALSE(parse_event("pong").has_value());
    EXPECT_FALSE(parse_event("").has_value());
    EXPECT_FALSE(parse_ticker(R"({"feed":"ticker"})").has_value());
    // a string bid must not read as a zero touch
    EXPECT_FALSE(parse_ticker(R"({"feed":"ticker","product_id":"PF_XBTUSD","bid":"1"})").has_value());
}

TEST(KrakenWire, TickerCarriesTouchAndFundingInOneFrame) {
    const auto frame = fixture("ws_ticker_pf.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->product_id, "PF_XBTUSD");
    EXPECT_EQ(wire->bid, "64110.0");
    EXPECT_EQ(wire->ask, "64111.0");
    EXPECT_EQ(wire->bid_size, "1.5787");
    EXPECT_EQ(wire->ask_size, "0.5");
    // epoch milliseconds as a json number
    EXPECT_EQ(wire->time_ms, 1786395177000ULL);
    EXPECT_EQ(wire->next_funding_rate_time_ms, 1786395600000ULL);
    EXPECT_FALSE(wire->suspended);
    // the token is handed over untouched; map.cpp interprets it
    EXPECT_EQ(wire->relative_funding_rate, "0.00001477885");
    EXPECT_EQ(wire->mark_price, "64113.93588534861");
    // websocket spellings; rest calls the first vol24h
    EXPECT_EQ(wire->volume, "5279.6877");
    EXPECT_EQ(wire->volume_quote, "341115306.3365");
}

TEST(KrakenWire, InverseTickerSpellsItsRateWithAnExponent) {
    // PI_ sends the relative rate in exponent form and PF_ as a plain decimal
    const auto frame = fixture("ws_ticker_pi.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->product_id, "PI_XBTUSD");
    EXPECT_EQ(wire->relative_funding_rate, "8.907125e-6");
    EXPECT_EQ(wire->bid, "63903.0");
    // on an inverse contract both columns are the same number
    EXPECT_EQ(wire->volume, "36162.0");
    EXPECT_EQ(wire->volume_quote, "36162.0");
}

TEST(KrakenWire, SnapshotAndLiveTradeShareOneParser) {
    // subscribing yields trade_snapshot first, then trade
    static TradeBuffer storage;

    const auto single = fixture("ws_trade.json");
    auto trades = parse_trades(single, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ((*trades)[0].product_id, "PF_XBTUSD");
    EXPECT_EQ((*trades)[0].price, "64111.0");
    EXPECT_EQ((*trades)[0].qty, "0.0757");
    EXPECT_TRUE((*trades)[0].aggressor_bought);
    EXPECT_EQ((*trades)[0].type, "fill");
    EXPECT_EQ((*trades)[0].time_ms, 1786395187475ULL);
    EXPECT_EQ((*trades)[0].seq, 258570ULL);

    const auto snapshot = fixture("ws_trade_snapshot.json");
    trades = parse_trades(snapshot, storage);
    ASSERT_TRUE(trades.has_value());
    EXPECT_EQ(trades->size(), 100U);
    EXPECT_EQ(trades->front().product_id, "PF_XBTUSD");
}

TEST(KrakenWire, TheTradeSnapshotIsHistoryOlderThanTheLiveTradeAfterIt) {
    static TradeBuffer storage;
    const auto live = fixture("ws_trade.json");
    const auto live_event = parse_event(live);
    ASSERT_TRUE(live_event.has_value());
    EXPECT_EQ(live_event->kind, EventKind::Trade);
    const auto live_trades = parse_trades(live, storage);
    ASSERT_TRUE(live_trades.has_value());
    const auto live_seq = live_trades->front().seq;
    const auto live_ms = live_trades->front().time_ms;

    const auto snapshot = fixture("ws_trade_snapshot.json");
    const auto event = parse_event(snapshot);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::TradeSnapshot);
    EXPECT_EQ(event->product_id, "PF_XBTUSD");
    const auto history = parse_trades(snapshot, storage);
    ASSERT_TRUE(history.has_value());
    for (const auto& print : *history) {
        EXPECT_LT(print.seq, live_seq);
        EXPECT_LE(print.time_ms, live_ms);
    }
}

TEST(KrakenWire, BookSnapshotIsWholeAndDeltasChainBySeq) {
    static LevelBuffer storage;
    const auto snapshot_frame = fixture("ws_book_snapshot.json");
    const auto snapshot = parse_book(snapshot_frame, storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    // kraken pushes the whole book
    EXPECT_EQ(snapshot->bids.size(), 1751U);
    EXPECT_EQ(snapshot->asks.size(), 1360U);
    EXPECT_EQ(snapshot->bids[0].price, "64110.0");
    EXPECT_EQ(snapshot->seq, 103307647ULL);
    const auto first_seq = snapshot->seq;

    // a delta is one level with its own side; the opposite side stays empty
    const auto delta_frame = fixture("ws_book_delta.json");
    const auto delta = parse_book(delta_frame, storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->snapshot);
    EXPECT_EQ(delta->seq, first_seq + 1);
    EXPECT_TRUE(delta->bids.empty());
    ASSERT_EQ(delta->asks.size(), 1U);
    EXPECT_EQ(delta->asks[0].price, "64133.0");

    const auto next_frame = fixture("ws_book_delta_next.json");
    const auto next = parse_book(next_frame, storage);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->seq, first_seq + 2);
    ASSERT_EQ(next->bids.size(), 1U);
    EXPECT_TRUE(next->asks.empty());
}

TEST(KrakenWire, RestEnvelopeReadsBothFailureShapes) {
    const auto ok_body = fixture("rest_ticker.json");
    const auto ok = parse_rest_envelope(ok_body);
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok->ok);
    EXPECT_TRUE(ok->error.empty());
    EXPECT_GT(ok->server_ms, 0ULL);

    // the code is a string name with a field suffix
    const auto bad_symbol_body = fixture("rest_error_bad_symbol.json");
    const auto bad_symbol = parse_rest_envelope(bad_symbol_body);
    ASSERT_TRUE(bad_symbol.has_value());
    EXPECT_FALSE(bad_symbol->ok);
    EXPECT_EQ(bad_symbol->error, "invalidArgument: symbol");

    // this answers under http 200
    const auto auth_body = fixture("rest_error_auth.json");
    const auto auth = parse_rest_envelope(auth_body);
    ASSERT_TRUE(auth.has_value());
    EXPECT_FALSE(auth->ok);
    EXPECT_EQ(auth->error, "authenticationError");

    // the routing-layer shape has no error string, so status is used
    const auto missing_body = fixture("rest_error_not_found.json");
    const auto missing = parse_rest_envelope(missing_body);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->ok);
    EXPECT_EQ(missing->error, "NOT_FOUND");
}

TEST(KrakenWire, ServerTimeIsIsoWithAVariableFraction) {
    // the fractional width varies, so the string is parsed, not sliced
    EXPECT_EQ(*parse_iso8601_ms("1970-01-01T00:00:00.000Z"), 0ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:52:16.918Z"), 1786395136918ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:56:08.64Z"), 1786395368640ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:52:17.246037Z"), 1786395137246ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:52:16Z"), 1786395136000ULL);
    EXPECT_FALSE(parse_iso8601_ms("2026-08-10").has_value());
    EXPECT_FALSE(parse_iso8601_ms("2026/08/10T20:52:16Z").has_value());
    EXPECT_FALSE(parse_iso8601_ms("1786395136918").has_value());
}

TEST(KrakenMap, NumberParserReadsBothSpellingsAndRefusesWhatItCannotHold) {
    EXPECT_EQ(parse_number_e9("64110.0")->raw, 64'110'000'000'000LL);
    EXPECT_EQ(parse_number_e9("0.5")->raw, 500'000'000LL);
    EXPECT_EQ(parse_number_e9("1")->raw, 1'000'000'000LL);
    EXPECT_EQ(parse_number_e9("-1.5")->raw, -1'500'000'000LL);
    // exponent form is the same value; kraken uses both for one field
    EXPECT_EQ(parse_number_e9("1e-3")->raw, parse_number_e9("0.001")->raw);
    EXPECT_EQ(parse_number_e9("8.907125e-6")->raw, parse_number_e9("0.000008907125")->raw);
    EXPECT_EQ(parse_number_e9("0.000000001")->raw, 1LL);
    EXPECT_EQ(parse_number_e9("1e2")->raw, 100'000'000'000LL);
    // below 1e-9 the value is refused, not flattened to zero
    EXPECT_FALSE(parse_number_e9("0.0000000001").has_value());
    EXPECT_FALSE(parse_number_e9("64113.93588534861").has_value());
    EXPECT_FALSE(parse_number_e9("").has_value());
    EXPECT_FALSE(parse_number_e9("abc").has_value());
    EXPECT_FALSE(parse_number_e9("1.2.3").has_value());
    EXPECT_FALSE(parse_number_e9("1e").has_value());
    EXPECT_FALSE(parse_number_e9("1e400").has_value());
}

TEST(KrakenMap, NegativePrecisionIsACoarserStepNotAFinerOne) {
    // precision is a negated exponent: 4 means 0.0001, -2 means 100
    EXPECT_EQ(step_from_precision(0)->raw, 1'000'000'000LL);      // step 1
    EXPECT_EQ(step_from_precision(4)->raw, 100'000LL);            // step 0.0001
    EXPECT_EQ(step_from_precision(9)->raw, 1LL);                  // step 0.000000001
    EXPECT_EQ(step_from_precision(-1)->raw, 10'000'000'000LL);    // step 10
    EXPECT_EQ(step_from_precision(-2)->raw, 100'000'000'000LL);   // step 100
    EXPECT_EQ(step_from_precision(-3)->raw, 1'000'000'000'000LL); // step 1000
    EXPECT_FALSE(step_from_precision(10).has_value());
    EXPECT_FALSE(step_from_precision(-10).has_value());
}

TEST(KrakenMap, CatalogueLoadsPerpsAndRefusesEverythingElse) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_instruments.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 6U);

    std::vector<std::string_view> loaded;
    for (const auto& row : *wire) {
        if (map_instrument(row, MarketType::Perp).has_value()) {
            loaded.push_back(row.symbol);
        }
    }
    // PF_XBTUSD linear, PI_XBTUSD inverse, PF_TURBOUSD for negative precision; the other three are refused
    ASSERT_EQ(loaded.size(), 3U);
    EXPECT_EQ(loaded[0], "PF_XBTUSD");
    EXPECT_EQ(loaded[1], "PI_XBTUSD");
    EXPECT_EQ(loaded[2], "PF_TURBOUSD");

    for (const auto& row : *wire) {
        if (row.symbol == "FI_XBTUSD_260925") {
            // futures_inverse covers PI_ and FI_; lastTradingTime separates them
            EXPECT_FALSE(row.last_trading_time.empty());
        } else if (row.symbol == "PF_PEPEUSD") {
            EXPECT_EQ(row.tick_size, "0.0000000001"); // finer than FixedE9 holds
        } else if (row.symbol == "PF_SOLUSD") {
            EXPECT_FALSE(row.tradeable);
        }
    }
    // spot is a separate api this adapter declines
    for (const auto& row : *wire) {
        EXPECT_FALSE(map_instrument(row, MarketType::Spot).has_value());
    }
}

TEST(KrakenMap, LinearAndInversePerpsStayDistinct) {
    const auto table = catalogue();
    const auto* linear = table.find("PF_XBTUSD");
    const auto* inverse = table.find("PI_XBTUSD");
    ASSERT_NE(linear, nullptr);
    ASSERT_NE(inverse, nullptr);
    // same underlying, different ticks and quantity units
    EXPECT_EQ(base_of(*linear).tick_size.raw, 1'000'000'000LL); // 1
    EXPECT_EQ(base_of(*inverse).tick_size.raw, 500'000'000LL);  // 0.5
    EXPECT_EQ(base_of(*linear).lot_size.raw, 100'000LL);        // 0.0001 BTC
    EXPECT_EQ(base_of(*inverse).lot_size.raw, 1'000'000'000LL); // 1 USD
    EXPECT_NE(base_of(*linear).id.packed, base_of(*inverse).id.packed);

    const auto* turbo = table.find("PF_TURBOUSD");
    ASSERT_NE(turbo, nullptr);
    EXPECT_EQ(base_of(*turbo).lot_size.raw, 100'000'000'000LL); // step 100
}

TEST(KrakenMap, OneTickerFrameYieldsBothTouchAndFunding) {
    const auto table = catalogue();
    const auto frame = fixture("ws_ticker_pf.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const UnixNanos now{7};

    const auto bbo = map_bbo(*wire, table, now);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 64110); // 64110.0 / tick 1
    EXPECT_EQ(bbo->ask.raw, 64111);
    EXPECT_EQ(bbo->bid_qty.raw, 15787); // 1.5787 / step 0.0001
    EXPECT_EQ(bbo->ask_qty.raw, 5000);
    EXPECT_EQ(bbo->ts_event.ns, 1786395177000ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, now.ns);

    const auto funding = map_funding(*wire, table, now);
    ASSERT_TRUE(funding.has_value());
    // relative_funding_rate, since the absolute field changes units by family
    EXPECT_EQ(funding->rate.raw, 14778); // 0.00001477885 truncated at 1e-9
    EXPECT_EQ(funding->next_funding_time.ns, 1786395600000ULL * 1'000'000ULL);
    // mark and index sit off the tick of 1 and are truncated onto it
    EXPECT_EQ(funding->mark_price.raw, 64113);     // 64113.93588534861
    EXPECT_EQ(funding->index_price.raw, 64108);    // 64108.28
    EXPECT_EQ(funding->predicted_rate.raw, 10006); // 0.000010006804166667
    // the period is not on the wire; funding is hourly
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->next_funding_time.ns % (3600ULL * 1'000'000'000ULL), 0U);
}

TEST(KrakenMap, InverseTickerMapsAgainstItsOwnSteps) {
    const auto table = catalogue();
    const auto frame = fixture("ws_ticker_pi.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 127806);    // 63903.0 / tick 0.5
    EXPECT_EQ(bbo->bid_qty.raw, 55146); // USD contracts, step 1
    const auto funding = map_funding(*wire, table, UnixNanos{1});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 8907);          // 8.907125e-6, exponent spelling
    EXPECT_EQ(funding->mark_price.raw, 128493);  // 64246.55472227413 on the 0.5 grid
    EXPECT_EQ(funding->index_price.raw, 128216); // 64108.28
    EXPECT_EQ(funding->predicted_rate.raw, 296408);
}

TEST(KrakenMap, TurnoverKeepsTheBaseColumnEmptyOnAnInverseContract) {
    const auto table = catalogue();
    const UnixNanos now{11};

    const auto linear_frame = fixture("ws_ticker_pf.json");
    const auto linear = parse_ticker(linear_frame);
    ASSERT_TRUE(linear.has_value());
    const auto linear_stats = map_ticker_stats(*linear, table, now);
    ASSERT_TRUE(linear_stats.has_value());
    EXPECT_EQ(linear_stats->volume_24h_base.raw, 5'279'687'700'000LL);    // 5279.6877 XBT
    EXPECT_EQ(linear_stats->volume_24h_quote.raw, 341'115'306'336'500LL); // 341115306.3365 USD
    EXPECT_EQ(linear_stats->ts_event.ns, 1786395177000ULL * 1'000'000ULL);
    EXPECT_EQ(linear_stats->ts_init.ns, now.ns);

    // PI_XBTUSD sends the same number in both columns; base must not take it
    const auto inverse_frame = fixture("ws_ticker_pi.json");
    const auto inverse = parse_ticker(inverse_frame);
    ASSERT_TRUE(inverse.has_value());
    const auto inverse_stats = map_ticker_stats(*inverse, table, now);
    ASSERT_TRUE(inverse_stats.has_value());
    EXPECT_EQ(inverse_stats->volume_24h_base.raw, 0);
    EXPECT_EQ(inverse_stats->volume_24h_quote.raw, 36'162'000'000LL);

    EXPECT_EQ(linear_stats->open_interest.raw, 2'277'932'600'000LL); // 2277.9326 XBT
    // 3492678 one-dollar contracts at the 64246.554722274 mark
    EXPECT_EQ(inverse_stats->open_interest.raw, 54'363'662'224LL);

    // neither a ticker_lite push nor an unknown product is a market that traded nothing
    auto lite = *linear;
    lite.volume = {};
    lite.volume_quote = {};
    EXPECT_FALSE(map_ticker_stats(lite, table, now).has_value());
    auto unknown = *linear;
    unknown.product_id = "PF_NOSUCHUSD";
    EXPECT_FALSE(map_ticker_stats(unknown, table, now).has_value());
}

TEST(KrakenMap, TradeKeepsTheAggressorSideAsSent) {
    const auto table = catalogue();
    static TradeBuffer storage;
    const auto frame = fixture("ws_trade.json");
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value());
    const auto trade = map_trade(trades->front(), table, UnixNanos{3});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 64111);
    EXPECT_EQ(trade->qty.raw, 757); // 0.0757 / step 0.0001
    // kraken states the taker's direction; no inversion
    EXPECT_EQ(trade->aggressor, Side::Buy);
}

TEST(KrakenMap, ValuesThatDoNotDivideAreDroppedNotRounded) {
    const auto table = catalogue();
    const auto frame = fixture("ws_ticker_pf.json");
    auto wire = *parse_ticker(frame);
    const std::string half_tick = "64110.5"; // tick is 1
    wire.bid = half_tick;
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    wire = *parse_ticker(frame);
    const std::string sub_lot = "1.57875"; // step is 0.0001
    wire.bid_size = sub_lot;
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    // an unknown product publishes nothing and is not an error
    wire = *parse_ticker(frame);
    const std::string unlisted = "PF_NOTLISTED";
    wire.product_id = unlisted;
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());
    EXPECT_FALSE(map_funding(wire, table, UnixNanos{1}).has_value());
}

TEST(KrakenMap, SuspendedAndEmptySidesPublishNoTouch) {
    const auto table = catalogue();
    const auto frame = fixture("ws_ticker_pf.json");
    auto wire = *parse_ticker(frame);
    wire.suspended = true;
    // a suspended market's quote is not tradeable
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    // kraken spells an empty side as zero, which must not publish as price 0
    wire = *parse_ticker(frame);
    const std::string zero = "0.0";
    wire.bid = zero;
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    // ticker_lite carries no funding fields at all; that is not a rate of zero.
    wire = *parse_ticker(frame);
    wire.relative_funding_rate = {};
    EXPECT_FALSE(map_funding(wire, table, UnixNanos{1}).has_value());
}

TEST(KrakenMap, AbsentVenueTimeFallsBackToIngestTime) {
    // a zero timestamp falls back to ingest time
    EXPECT_EQ(ms_to_unix(0, UnixNanos{42}).ns, 42ULL);
    EXPECT_EQ(ms_to_unix(1786395177000ULL, UnixNanos{42}).ns, 1786395177000ULL * 1'000'000ULL);
}

// both the frame and the level buffer must outlive the wire struct
struct BookFrame {
    std::string text;
    std::unique_ptr<LevelBuffer> storage{std::make_unique<LevelBuffer>()};
};

WireBook book_of(BookFrame& frame, std::string_view name) {
    frame.text = fixture(name);
    const auto wire = parse_book(frame.text, *frame.storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireBook{});
}

Book book_for(const exec::InstrumentTable& table) {
    const auto* instrument = table.find("PF_XBTUSD");
    EXPECT_NE(instrument, nullptr);
    const auto& base = base_of(*instrument);
    return Book{base.id, base.tick_size, base.lot_size};
}

// bids descend, asks ascend, no cross, every quantity real
void expect_coherent(const BookSnapshot& out) {
    ASSERT_GT(out.bid_count, 0U);
    ASSERT_GT(out.ask_count, 0U);
    EXPECT_LE(out.bid_count, k_book_levels);
    EXPECT_LE(out.ask_count, k_book_levels);
    EXPECT_LT(out.bids[0].price.raw, out.asks[0].price.raw);
    for (std::uint32_t i = 1; i < out.bid_count; ++i) {
        EXPECT_LT(out.bids[i].price.raw, out.bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].price.raw, out.asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < out.bid_count; ++i) {
        EXPECT_GT(out.bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].qty.raw, 0) << i;
    }
}

TEST(KrakenBook, ADeltaBeforeAnySnapshotPublishesNothing) {
    // one delta is one level, not a book
    const auto table = catalogue();
    auto book = book_for(table);
    BookFrame delta;
    EXPECT_EQ(book.apply(book_of(delta, "ws_book_delta.json")), BookResult::Ignored);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(KrakenBook, SnapshotAndDeltasMakeABookSharedMemoryCanTake) {
    const auto table = catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame first;
    BookFrame second;

    const auto snapshot = book_of(snapshot_frame, "ws_book_snapshot.json");
    EXPECT_EQ(book.apply(snapshot), BookResult::Resynced);
    const auto d1 = book_of(first, "ws_book_delta.json");
    EXPECT_EQ(d1.seq, snapshot.seq + 1) << "seq steps by exactly one";
    EXPECT_EQ(book.apply(d1), BookResult::Applied);
    const auto d2 = book_of(second, "ws_book_delta_next.json");
    EXPECT_EQ(book.apply(d2), BookResult::Applied);

    const auto out = book.snapshot(UnixNanos{7}, UnixNanos{9});
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    // the held book keeps every level; shm gets the top cut
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(book.bids().size(), 1751U);
    EXPECT_EQ(book.asks().size(), 1360U);
    EXPECT_EQ(out->venue_seq, d2.seq) << "seq is the forensic handle";
    EXPECT_EQ(out->ts_event.ns, 7ULL);
    EXPECT_EQ(out->ts_init.ns, 9ULL);

    const auto at_ask = std::ranges::find_if(book.asks(),
                                             [](const Book::Level& level) { return level.price.raw == 64'133; });
    ASSERT_NE(at_ask, book.asks().end());
    EXPECT_EQ(at_ask->qty.raw, 28'971);
    const auto at_bid = std::ranges::find_if(book.bids(),
                                             [](const Book::Level& level) { return level.price.raw == 64'100; });
    ASSERT_NE(at_bid, book.bids().end());
    EXPECT_EQ(at_bid->qty.raw, 2475);
}

TEST(KrakenBook, ASkippedSeqDropsTheBookUntilTheNextSnapshot) {
    const auto table = catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame first;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot.json")), BookResult::Resynced);

    auto skipped = book_of(first, "ws_book_delta.json");
    skipped.seq += 1; // one frame of the chain went missing

    EXPECT_EQ(book.apply(skipped), BookResult::GapDetected);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(KrakenBook, ARedeliveredFrameIsIgnoredRatherThanTreatedAsAGap) {
    const auto table = catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame first;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot.json")), BookResult::Resynced);
    const auto d1 = book_of(first, "ws_book_delta.json");
    EXPECT_EQ(book.apply(d1), BookResult::Applied);

    // sizes are absolute, so a repeat is ignored without a resync
    EXPECT_EQ(book.apply(d1), BookResult::Ignored);
    EXPECT_EQ(book.gaps(), 0U);
    EXPECT_TRUE(book.synced());
    EXPECT_EQ(book.last_seq(), d1.seq);
}

TEST(KrakenBook, AQuantityOfZeroDeletesTheLevel) {
    const auto table = catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    const auto snapshot = book_of(snapshot_frame, "ws_book_snapshot.json");
    EXPECT_EQ(book.apply(snapshot), BookResult::Resynced);

    const auto before = book.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->asks[0].price.raw, 64'111);
    const auto next_up = before->asks[1].price.raw;

    const std::array<WireLevel, 1> gone{WireLevel{.price = "64111.0", .qty = "0"}};
    WireBook lifted{};
    lifted.product_id = "PF_XBTUSD";
    lifted.seq = snapshot.seq + 1;
    lifted.asks = gone;
    EXPECT_EQ(book.apply(lifted), BookResult::Applied);

    const auto after = book.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->asks[0].price.raw, next_up) << "a zero deletes, it does not rest at zero";
    EXPECT_EQ(after->ask_count, before->ask_count);
}

TEST(KrakenBook, ALevelOffTheStepInvalidatesTheWholeBook) {
    // no level may be dropped in a delta book
    const auto table = catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    const auto snapshot = book_of(snapshot_frame, "ws_book_snapshot.json");
    EXPECT_EQ(book.apply(snapshot), BookResult::Resynced);

    const std::array<WireLevel, 1> bad{WireLevel{.price = "64110.5", .qty = "1.0"}};
    WireBook off_tick{};
    off_tick.product_id = "PF_XBTUSD";
    off_tick.seq = snapshot.seq + 1;
    off_tick.bids = bad;
    EXPECT_EQ(book.apply(off_tick), BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());

    // a size finer than the 0.0001 step is refused the same way
    auto again = book_for(table);
    EXPECT_EQ(again.apply(snapshot), BookResult::Resynced);
    const std::array<WireLevel, 1> sub_lot{WireLevel{.price = "64110.0", .qty = "1.00005"}};
    WireBook fine{};
    fine.product_id = "PF_XBTUSD";
    fine.seq = snapshot.seq + 1;
    fine.bids = sub_lot;
    EXPECT_EQ(again.apply(fine), BookResult::BadLevel);
    EXPECT_FALSE(again.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(KrakenErrors, ClassificationReadsTheNameNotTheWholeString) {
    // the name carries a field suffix live
    EXPECT_EQ(classify("invalidArgument: symbol"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("authenticationError"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify("apiLimitExceeded"), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify("marketUnavailable"), exec::ErrorClass::Retryable);
    // nonce skew, not bad credentials; a re-signed payload succeeds
    EXPECT_EQ(classify("nonceBelowThreshold"), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify("NOT_FOUND"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("SERVICE_UNAVAILABLE"), exec::ErrorClass::Retryable);
    // unknown codes default to terminal
    EXPECT_EQ(classify("somethingKrakenAddsLater"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(""), exec::ErrorClass::Terminal);
    EXPECT_TRUE(is_benign_cancel_failure("orderForEditNotFound"));
    EXPECT_FALSE(is_benign_cancel_failure("invalidArgument: orderId"));
}

TEST(KrakenWs, ThreeKindsResolveToTheOneTickerFeed) {
    // touch, mark and funding share one ticker subscription
    EXPECT_EQ(feed_for(exec::SubKind::Bbo), "ticker");
    EXPECT_EQ(feed_for(exec::SubKind::Funding), "ticker");
    EXPECT_EQ(feed_for(exec::SubKind::MarkPrice), "ticker");
    EXPECT_EQ(feed_for(exec::SubKind::Trades), "trade");
    EXPECT_EQ(feed_for(exec::SubKind::Depth), "book");
    EXPECT_TRUE(feed_for(exec::SubKind::Kline).empty());
}

TEST(KrakenWs, TheWholeLiveCatalogueSubscribesInFewerFramesThanTheSendQueueHolds) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_instruments_live.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());

    std::vector<OwnedSubscription> set;
    std::size_t perps = 0;
    for (const auto& row : *wire) {
        if (!map_instrument(row, MarketType::Perp).has_value()) {
            continue;
        }
        ++perps;
        for (const auto kind :
             {exec::SubKind::Bbo, exec::SubKind::Trades, exec::SubKind::Depth, exec::SubKind::Funding}) {
            add_subscription(set, feed_for(kind), row.symbol);
        }
    }
    ASSERT_GT(perps, 256U);
    ASSERT_EQ(set.size(), perps * 3);

    std::vector<Subscription> views;
    for (const auto& entry : set) {
        views.push_back({.feed = entry.feed, .product_id = entry.product_id});
    }
    Protocol protocol;
    std::size_t sent = 0;
    std::size_t frames = 0;
    std::size_t tickers = 0;
    while (sent < views.size()) {
        const auto frame = protocol.subscribe_frame(
            std::span<const Subscription>{views}.subspan(sent, std::min<std::size_t>(64, views.size() - sent)));
        ASSERT_TRUE(frame.has_value());
        if (views[sent].feed == "ticker") {
            tickers += frame->consumed;
        }
        sent += frame->consumed;
        ++frames;
    }
    EXPECT_EQ(tickers, perps);
    EXPECT_EQ(frames, 3 * ((perps + 63) / 64));
    EXPECT_LT(frames, net::WsConfig{}.send_queue_capacity);
}

TEST(KrakenWs, SubscribeFrameNamesOneFeedAndManyProducts) {
    Protocol protocol;
    const std::array<Subscription, 3> mixed{Subscription{.feed = "ticker", .product_id = "PF_XBTUSD"},
                                            Subscription{.feed = "ticker", .product_id = "PI_XBTUSD"},
                                            Subscription{.feed = "trade", .product_id = "PF_XBTUSD"}};

    // one feed per frame with an array of products; the caller resumes from consumed
    const auto first = protocol.subscribe_frame(mixed);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->consumed, 2U);
    EXPECT_EQ(first->text, R"({"event":"subscribe","feed":"ticker","product_ids":["PF_XBTUSD","PI_XBTUSD"]})");

    const auto second = protocol.subscribe_frame(std::span<const Subscription>{mixed}.subspan(first->consumed));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->consumed, 1U);
    EXPECT_EQ(second->text, R"({"event":"subscribe","feed":"trade","product_ids":["PF_XBTUSD"]})");

    const auto off = protocol.unsubscribe_frame(std::span<const Subscription>{mixed}.subspan(2));
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"event":"unsubscribe","feed":"trade","product_ids":["PF_XBTUSD"]})");

    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
    // an empty feed would subscribe to "" and earn an alert
    const std::array<Subscription, 1> declined{Subscription{.feed = "", .product_id = "PF_XBTUSD"}};
    EXPECT_FALSE(protocol.subscribe_frame(declined).has_value());
}

TEST(KrakenRest, TargetsMatchTheEndpointsTheFixturesCameFrom) {
    RequestBuilder builder;
    EXPECT_EQ(rest_host(), "futures.kraken.com");
    EXPECT_EQ(builder.instruments().target, "/derivatives/api/v3/instruments");
    // symbol is a path segment on tickers and a query parameter on orderbook
    EXPECT_EQ(builder.ticker("PF_XBTUSD").target, "/derivatives/api/v3/tickers/PF_XBTUSD");
    EXPECT_EQ(builder.orderbook("PI_XBTUSD").target, "/derivatives/api/v3/orderbook?symbol=PI_XBTUSD");
    EXPECT_EQ(builder.instruments().method, net::HttpMethod::Get);
}

// 64 bytes 0x00..0x3f, base64-encoded, so the decoded key is inspectable
constexpr std::string_view
    k_test_secret = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+Pw==";

// the message every signed request in this file reduces to
constexpr std::string_view
    k_send_message = "orderType=lmt&symbol=PI_XBTUSD&side=buy&size=1&limitPrice=94001690000000000/api/v3/sendorder";

TEST(KrakenSign, TheSecretIsBase64DecodedBeforeItKeysTheHmac) {
    // computed independently in python: b64decode, sha256, hmac-sha512, b64encode
    std::array<char, k_authent_bytes> out{};
    const auto signature = authent(k_test_secret, k_send_message, out);
    EXPECT_EQ(signature,
              "AuijpQ61ixs1MRiL1sTMNvtagw/9ziKaXrt7OXCjkpHGKX5I6lC2NKfGf+ItOGNDQFIdetXNHNBIxe7Q"
              "iBqekw==");
    EXPECT_EQ(signature.size(), k_authent_bytes);

    // the ws challenge runs the same chain over the uuid alone
    const auto signed_challenge = authent(k_test_secret, "226aee50-88fc-4618-a42a-34f7709570b2", out);
    EXPECT_EQ(signed_challenge,
              "uI506uuxHesOy5ARmPf7tntMqCAGiAf+W4q69I/S6UeO0gQTVLJHuIgfkxI5pUYpP5fqWE9mqu3ix8ov"
              "2uHO9w==");

    // invalid base64 yields no signature
    EXPECT_TRUE(authent("not base64!", k_send_message, out).empty());
    EXPECT_TRUE(authent("", k_send_message, out).empty());
    // an output buffer that cannot hold 88 characters is refused
    std::array<char, 8> small{};
    EXPECT_TRUE(authent(k_test_secret, k_send_message, small).empty());
}

InstrumentId id_of(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    return instrument == nullptr ? InstrumentId{} : base_of(*instrument).id;
}

// PI_XBTUSD: tick 0.5, lot 1. 18800 ticks is 9400, one lot is one contract.
OrderCmd limit_buy(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = id_of(table, "PI_XBTUSD");
    cmd.client_order_id = ClientOrderId::make(7, 11);
    cmd.price = PriceTicks::from_raw(18800);
    cmd.qty = QtyLots::from_raw(1);
    return cmd;
}

void load_credentials(net::Signer& signer) {
    signer.set_api_key("k-public");
    ASSERT_TRUE(signer.secret().assign(k_test_secret).has_value());
}

constexpr UnixNanos k_now{1'690'000'000'000ULL * 1'000'000ULL};

TEST(KrakenRest, ASignedPostSignsTheBodyItActuallySends) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto request = builder.create_order(limit_buy(table), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    // the target carries /derivatives; the signature does not
    EXPECT_EQ(request->target, "/derivatives/api/v3/sendorder");
    EXPECT_EQ(request->body, "orderType=lmt&symbol=PI_XBTUSD&side=buy&size=1&limitPrice=9400");
    EXPECT_EQ(request->content_type, "application/x-www-form-urlencoded");
    EXPECT_EQ(builder.last_sign_message(), k_send_message);
    EXPECT_EQ(builder.last_authent(),
              "AuijpQ61ixs1MRiL1sTMNvtagw/9ziKaXrt7OXCjkpHGKX5I6lC2NKfGf+ItOGNDQFIdetXNHNBIxe7Q"
              "iBqekw==");

    ASSERT_EQ(request->headers.size(), 3U);
    EXPECT_EQ(request->headers[0].name, "APIKey");
    EXPECT_EQ(request->headers[0].value, "k-public");
    EXPECT_EQ(request->headers[1].name, "Nonce");
    EXPECT_EQ(request->headers[1].value, "1690000000000");
    EXPECT_EQ(request->headers[2].name, "Authent");
    EXPECT_EQ(request->headers[2].value, builder.last_authent());
}

TEST(KrakenRest, NonceStrictlyIncreasesWithinOneMillisecond) {
    // two orders in the same millisecond must get distinct nonces
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    ASSERT_TRUE(builder.create_order(limit_buy(table), table, k_now).has_value());
    const auto first = std::string{builder.last_sign_message()};
    ASSERT_TRUE(builder.create_order(limit_buy(table), table, k_now).has_value());
    EXPECT_NE(builder.last_sign_message(), first);
    EXPECT_NE(builder.last_sign_message().find("1690000000001"), std::string_view::npos);
}

TEST(KrakenRest, OrderTypeAndTimeInForceAreOneField) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = limit_buy(table);
    cmd.tif = TimeInForce::Ioc;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_TRUE(builder.last_sign_message().starts_with("orderType=ioc&"));

    cmd.tif = TimeInForce::Fok;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_TRUE(builder.last_sign_message().starts_with("orderType=fok&"));

    // post-only is an order type and wins over ioc
    cmd.flags = OrderFlags::PostOnly;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_TRUE(builder.last_sign_message().starts_with("orderType=post&"));

    // a market order carries no limitPrice
    cmd.order_type = OrderType::Market;
    cmd.flags = OrderFlags::ReduceOnly;
    const auto market = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(market.has_value());
    EXPECT_EQ(market->body, "orderType=mkt&symbol=PI_XBTUSD&side=buy&size=1&reduceOnly=true") << market->body;
}

TEST(KrakenRest, CancelSpellsTheTargetOrderIdOneWayAndEditSpellsItTheOther) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = limit_buy(table);
    cmd.kind = CmdKind::Cancel;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("cb4e34f6-4eb3-4d4b-9724-4c3035b99d47");
    const auto cancel = builder.cancel_order(cmd, table, k_now);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->target, "/derivatives/api/v3/cancelorder");
    EXPECT_EQ(cancel->body, "order_id=cb4e34f6-4eb3-4d4b-9724-4c3035b99d47");

    // editorder spells it orderId
    cmd.kind = CmdKind::Amend;
    const auto edit = builder.amend_order(cmd, table, k_now);
    ASSERT_TRUE(edit.has_value());
    EXPECT_EQ(edit->target, "/derivatives/api/v3/editorder");
    EXPECT_EQ(edit->body, "orderId=cb4e34f6-4eb3-4d4b-9724-4c3035b99d47&size=1&limitPrice=9400");

    // neither id, so nothing is sent
    cmd.target_exchange_order_id = {};
    cmd.exchange_client_order_id = {};
    EXPECT_EQ(builder.cancel_order(cmd, table, k_now).error(), BuildError::NoOrderTarget);

    // A client order id is the fallback, on both endpoints.
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01J9ZQ8V3T4W5X6Y7Z8A9B0C1D");
    const auto by_client = builder.cancel_order(cmd, table, k_now);
    ASSERT_TRUE(by_client.has_value());
    EXPECT_EQ(by_client->body, "cliOrdId=01J9ZQ8V3T4W5X6Y7Z8A9B0C1D");
}

TEST(KrakenRest, GetsCarryTheirParametersInTheQueryAndSignTheSameText) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto open = builder.open_orders(k_now);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->method, net::HttpMethod::Get);
    EXPECT_EQ(open->target, "/derivatives/api/v3/openorders");
    EXPECT_TRUE(open->body.empty());
    EXPECT_EQ(builder.last_sign_message(), "1690000000000/api/v3/openorders");
    EXPECT_EQ(builder.last_authent(),
              "zkNjmi34ExjThETZGFFdBoBBdbql2qiZXts/ISOj5QHsRRqVt+lc6aQHUXiIecDrto8mErJ70i3iL4gI"
              "f6EtPg==");

    const auto positions = builder.positions(k_now);
    ASSERT_TRUE(positions.has_value());
    EXPECT_EQ(positions->target, "/derivatives/api/v3/openpositions");

    const auto newest = builder.fills({}, k_now);
    ASSERT_TRUE(newest.has_value());
    EXPECT_EQ(newest->target, "/derivatives/api/v3/fills");

    // the cursor is echoed verbatim
    const auto paged = builder.fills("2020-07-21T12:41:52.790Z", k_now);
    ASSERT_TRUE(paged.has_value());
    EXPECT_EQ(paged->target, "/derivatives/api/v3/fills?lastFillTime=2020-07-21T12:41:52.790Z");
    EXPECT_TRUE(paged->body.empty());
    EXPECT_EQ(builder.last_sign_message(), "lastFillTime=2020-07-21T12:41:52.790Z1690000000003/api/v3/fills");
}

TEST(KrakenRest, MarketRefusalIsNamedBeforeTheInstrumentTableIsEverRead) {
    // the table is empty on purpose: the spot refusal must not depend on it
    const exec::InstrumentTable empty;
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    for (const auto& [market, expected] : {std::pair{MarketType::Spot, BuildError::SpotUnsupported},
                                           std::pair{MarketType::Future, BuildError::DatedFuturesUnsupported},
                                           std::pair{MarketType::Option, BuildError::OptionsUnsupported}}) {
        OrderCmd cmd{};
        cmd.instrument = InstrumentId::make(VenueId::Kraken, market, SymbolId{3});
        EXPECT_EQ(builder.create_order(cmd, empty, k_now).error(), expected);
        EXPECT_EQ(builder.cancel_order(cmd, empty, k_now).error(), expected);
        EXPECT_EQ(builder.amend_order(cmd, empty, k_now).error(), expected);
        EXPECT_EQ(market_refusal(market), expected);
    }
    EXPECT_FALSE(market_refusal(MarketType::Perp).has_value());

    // a perp id missing from the table is a different answer
    OrderCmd perp{};
    perp.instrument = InstrumentId::make(VenueId::Kraken, MarketType::Perp, SymbolId{9});
    EXPECT_EQ(builder.create_order(perp, empty, k_now).error(), BuildError::UnknownInstrument);
}

TEST(KrakenRest, WithoutCredentialsNothingIsBuiltAndNothingIsSigned) {
    const auto table = catalogue();
    net::Signer signer;
    SignedRequestBuilder builder{signer};
    EXPECT_EQ(builder.create_order(limit_buy(table), table, k_now).error(), BuildError::NotSigned);
    EXPECT_EQ(builder.open_orders(k_now).error(), BuildError::NotSigned);
}

TEST(KrakenWire, TheThreeVerdictObjectsAnswerUnderThreeKeysAndTwoIdSpellings) {
    const auto placed = fixture("rest_send_order_placed.json");
    auto status = parse_order_status(placed, StatusKind::Send);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status, "placed");
    EXPECT_EQ(status->order_id, "179f9af8-e45e-469d-b3e9-2fd4675cb7d0");
    EXPECT_NE(status->received_ms, 0U);
    EXPECT_TRUE(is_send_accepted(status->status));

    // the envelope says success for refused orders too; status is the verdict
    const auto rejected = fixture("rest_send_order_rejected.json");
    const auto envelope = parse_rest_envelope(rejected);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_TRUE(envelope->ok);
    status = parse_order_status(rejected, StatusKind::Send);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status, "iocWouldNotExecute");
    EXPECT_FALSE(is_send_accepted(status->status));

    const auto cancelled = fixture("rest_cancel_order.json");
    status = parse_order_status(cancelled, StatusKind::Cancel);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status, "cancelled");
    EXPECT_TRUE(is_cancel_accepted(status->status));

    // editorder answers orderId, sendorder order_id
    const auto edited = fixture("rest_edit_order.json");
    status = parse_order_status(edited, StatusKind::Edit);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status, "edited");
    EXPECT_EQ(status->order_id, "022774bc-2c4a-4f26-9317-436c8d85746d");
    EXPECT_TRUE(is_edit_accepted(status->status));
}

TEST(KrakenMap, AnOpenOrderRowStatesWhatIsLeftAndWhatTraded) {
    const auto table = catalogue();
    const auto body = fixture("rest_open_orders.json");
    static thread_local OpenOrderBuffer storage;
    const auto orders = parse_open_orders(body, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 3U);

    const auto untouched = map_open_order((*orders)[0], table);
    ASSERT_TRUE(untouched.has_value());
    EXPECT_EQ(untouched->status, OrderStatus::Accepted);
    EXPECT_EQ(untouched->side, Side::Sell);
    EXPECT_EQ(untouched->price.raw, 21280); // 10640 / 0.5
    EXPECT_EQ(untouched->qty.raw, 304);
    EXPECT_EQ(untouched->leaves.raw, 304);

    // the original size is filledSize + unfilledSize
    const auto partial = map_open_order((*orders)[1], table);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(partial->qty.raw, 1501);
    EXPECT_EQ(partial->leaves.raw, 1201);
    EXPECT_EQ(partial->exchange_client_order_id.view(), "01J9ZQ8V3T4W5X6Y7Z8A9B0C1D");

    // A symbol this adapter never loaded yields nothing rather than a report
    // against instrument zero.
    EXPECT_FALSE(map_open_order((*orders)[2], table).has_value());
}

TEST(KrakenMap, FillsCarryTheirDedupeKeyAndOnlyTwoFillTypesAreASideOfTheBook) {
    const auto table = catalogue();
    const auto body = fixture("rest_fills.json");
    static thread_local FillBuffer storage;
    const auto fills = parse_fills(body, storage);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->size(), 3U);

    const auto maker = map_rest_fill((*fills)[0], table);
    ASSERT_TRUE(maker.has_value());
    EXPECT_EQ(maker->liquidity, Liquidity::Maker);
    EXPECT_EQ(maker->side, Side::Buy);
    EXPECT_EQ(maker->price.raw, 18800); // 9400 / 0.5
    EXPECT_EQ(maker->qty.raw, 5490);
    EXPECT_EQ(maker->exec_id.view(), "3d57ed09-fbd6-44f1-8e8b-b10e551c5e73");
    EXPECT_EQ(maker->exchange_client_order_id.view(), "01J9ZQ8V3T4W5X6Y7Z8A9B0C1D");

    EXPECT_EQ(map_rest_fill((*fills)[1], table)->liquidity, Liquidity::Taker);
    // a liquidation has no maker or taker side
    EXPECT_EQ(map_rest_fill((*fills)[2], table)->liquidity, Liquidity::Unknown);
}

TEST(KrakenMap, APositionKeepsItsSizeEvenWhenNoTickDividesItsAverageEntry) {
    const auto table = catalogue();
    const auto body = fixture("rest_open_positions.json");
    static thread_local PositionBuffer storage;
    const auto positions = parse_positions(body, storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 3U);

    // the average does not divide the 0.5 tick; the size is reported, the average is not
    const auto shrt = map_position((*positions)[0], table);
    ASSERT_TRUE(shrt.has_value());
    EXPECT_EQ(shrt->net_qty.raw, -10000);
    EXPECT_TRUE(shrt->avg_entry.is_undef());

    const auto lng = map_position((*positions)[1], table);
    ASSERT_TRUE(lng.has_value());
    EXPECT_EQ(lng->net_qty.raw, 5000); // 0.5 BTC at a 0.0001 step
    EXPECT_EQ(lng->avg_entry.raw, 64000);

    // A dated future is refused by the catalogue, so a position on one has no
    // instrument to attach to.
    EXPECT_FALSE(map_position((*positions)[2], table).has_value());
}

TEST(KrakenMap, ADeltaReasonIsTheWholeVerdict) {
    // kraken spells two reasons as sentences; match them exactly
    EXPECT_EQ(effect_of("new_placed_order_by_user", false), DeltaEffect::Accepted);
    EXPECT_EQ(effect_of("limit_order_from_stop", false), DeltaEffect::Accepted);
    EXPECT_EQ(effect_of("stop_order_triggered", false), DeltaEffect::Triggered);
    EXPECT_EQ(effect_of("cancelled_by_user", true), DeltaEffect::Canceled);
    EXPECT_EQ(effect_of("dead_man_switch", true), DeltaEffect::Canceled);
    EXPECT_EQ(effect_of("liquidation", true), DeltaEffect::Canceled);
    EXPECT_EQ(effect_of("post_order_failed_because_it_would_filled", true), DeltaEffect::Rejected);
    EXPECT_EQ(effect_of("ioc_order_failed_because_it_would_not_be_executed", true), DeltaEffect::Rejected);
    EXPECT_EQ(effect_of("would_execute_self", true), DeltaEffect::Rejected);
    EXPECT_EQ(effect_of("not_enough_margin", true), DeltaEffect::Rejected);
    // the fills feed owns every fill
    EXPECT_EQ(effect_of("full_fill", true), DeltaEffect::Silent);
    EXPECT_EQ(effect_of("partial_fill", false), DeltaEffect::Silent);
    // A failed edit is not a departure; editorder already reported it.
    EXPECT_EQ(effect_of("order_for_edit_not_found", true), DeltaEffect::Silent);
    EXPECT_EQ(effect_of("something_kraken_adds_later", false), DeltaEffect::Silent);
}

TEST(KrakenMap, TheOpenOrdersFeedStatesWhatREMAINSNotWhatWasAsked) {
    const auto table = catalogue();
    const auto snapshot = fixture("ws_open_orders_snapshot.json");
    static thread_local OrderSnapshotBuffer storage;
    const auto orders = parse_order_snapshot(snapshot, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 2U);
    EXPECT_EQ((*orders)[0].buy, false); // direction 1 is a sell
    EXPECT_EQ((*orders)[1].buy, true);

    const auto placed = fixture("ws_open_orders_new.json");
    auto delta = parse_order_delta(placed);
    ASSERT_TRUE(delta.has_value());
    ASSERT_TRUE(delta->has_order);
    EXPECT_EQ(effect_of(delta->reason, delta->is_cancel), DeltaEffect::Accepted);
    auto event = map_ws_order(delta->order, OrderEventType::Accepted, table, UnixNanos{99});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->side, Side::Sell);
    EXPECT_EQ(event->price.raw, 21280);
    EXPECT_EQ(event->qty.raw, 304);
    EXPECT_EQ(event->leaves_qty.raw, 304);
    EXPECT_TRUE(has_flag(event->flags, OrderFlags::ReduceOnly));
    EXPECT_EQ(event->exchange_client_order_id.view(), "01J9ZQ8V3T4W5X6Y7Z8A9B0C1D");

    // qty is what is left and filled is cumulative; the original is the sum
    const auto partial = fixture("ws_open_orders_partial.json");
    delta = parse_order_delta(partial);
    ASSERT_TRUE(delta.has_value());
    ASSERT_TRUE(delta->has_order);
    event = map_ws_order(delta->order, OrderEventType::Accepted, table, UnixNanos{99});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->qty.raw, 304);
    EXPECT_EQ(event->leaves_qty.raw, 204);

    // a removal names only an order id
    const auto removal = fixture("ws_open_orders_cancel.json");
    delta = parse_order_delta(removal);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->has_order);
    EXPECT_EQ(delta->order_id, "59302619-41d2-4f0b-941f-7e7914760ad3");
    EXPECT_EQ(effect_of(delta->reason, delta->is_cancel), DeltaEffect::Canceled);

    const auto refused = fixture("ws_open_orders_post_refused.json");
    delta = parse_order_delta(refused);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(effect_of(delta->reason, delta->is_cancel), DeltaEffect::Rejected);
}

TEST(KrakenMap, AFillCarriesTheLeavesTheFeedStatesAndAFeeInTheSettlementCurrency) {
    const auto table = catalogue();
    const auto snapshot = fixture("ws_fills_snapshot.json");
    static thread_local WsFillBuffer storage;
    auto fills = parse_ws_fills(snapshot, storage);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->size(), 1U);
    auto event = map_ws_fill((*fills)[0], table, UnixNanos{99});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    EXPECT_EQ(event->liquidity, Liquidity::Maker);
    EXPECT_EQ(event->price.raw, 21875); // 10937.5 / 0.5
    EXPECT_EQ(event->qty.raw, 5000);
    EXPECT_EQ(event->leaves_qty.raw, 0);
    // a maker rebate is negative with eleven decimals, truncated at 1e-9
    EXPECT_EQ(event->fee.raw, -91428);

    const auto partial = fixture("ws_fills_partial.json");
    fills = parse_ws_fills(partial, storage);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->size(), 1U);
    event = map_ws_fill((*fills)[0], table, UnixNanos{99});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->qty.raw, 100);
    // remaining_order_qty is the venue's own count of what is still working.
    EXPECT_EQ(event->leaves_qty.raw, 204);
    EXPECT_EQ(event->liquidity, Liquidity::Taker);
    EXPECT_EQ(event->fee.raw, 18285);
    EXPECT_EQ(event->exec_id.view(), "9f5e2b1a-0d43-4b21-9c8f-6a2e13d4c7b5");
}

TEST(KrakenWs, ThePrivateSubscribeCarriesTheSignedChallengeAndNeverAnEmptyOne) {
    const auto frame = fixture("ws_challenge.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Challenge);
    const auto challenge = parse_challenge(frame);
    ASSERT_TRUE(challenge.has_value());
    EXPECT_EQ(challenge->message, "226aee50-88fc-4618-a42a-34f7709570b2");

    Protocol protocol;
    const auto ask = protocol.challenge_request_frame("k-public");
    ASSERT_TRUE(ask.has_value());
    EXPECT_EQ(ask->text, R"({"event":"challenge","api_key":"k-public"})");

    const auto subscribe = protocol.private_subscribe_frame(k_feed_open_orders, "k-public", challenge->message, "sig");
    ASSERT_TRUE(subscribe.has_value());
    EXPECT_EQ(subscribe->text,
              R"({"event":"subscribe","feed":"open_orders","api_key":"k-public",)"
              R"("original_challenge":"226aee50-88fc-4618-a42a-34f7709570b2",)"
              R"("signed_challenge":"sig"})");

    // a missing credential would subscribe the public form, answered by an alert
    EXPECT_FALSE(protocol.private_subscribe_frame(k_feed_fills, "", "c", "s").has_value());
    EXPECT_FALSE(protocol.private_subscribe_frame(k_feed_fills, "k", "", "s").has_value());
    EXPECT_FALSE(protocol.private_subscribe_frame(k_feed_fills, "k", "c", "").has_value());
    EXPECT_FALSE(protocol.challenge_request_frame("").has_value());
}

TEST(KrakenErrors, TheOrderVerdictVocabularyIsNotTheErrorVocabulary) {
    // error names a request failure, status the order outcome
    EXPECT_TRUE(is_send_accepted("placed"));
    EXPECT_TRUE(is_send_accepted("filled"));
    EXPECT_TRUE(is_send_accepted("partiallyFilled"));
    EXPECT_FALSE(is_send_accepted("postWouldExecute"));
    EXPECT_FALSE(is_send_accepted("insufficientAvailableFunds"));
    EXPECT_FALSE(is_send_accepted("cancelled"));

    // an order already gone is an accepted cancel
    EXPECT_TRUE(is_cancel_accepted("cancelled"));
    EXPECT_TRUE(is_cancel_accepted("filled"));
    EXPECT_TRUE(is_cancel_accepted("notFound"));
    EXPECT_FALSE(is_cancel_accepted("placed"));

    EXPECT_TRUE(is_edit_accepted("edited"));
    EXPECT_FALSE(is_edit_accepted("orderForEditNotFound"));
    EXPECT_FALSE(is_edit_accepted(""));
}

TEST(KrakenCatalogue, InversePerpsKeepASymbolAndMarketOfTheirOwn) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_instruments_inverse_pairs.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *wire) {
        auto instrument = map_instrument(row, MarketType::Perp);
        ASSERT_TRUE(instrument.has_value()) << row.symbol;
        classify(VenueId::Kraken, *instrument);
        ASSERT_TRUE(table.add(*instrument).has_value());
    }
    ASSERT_EQ(table.size(), 8U);

    const auto& inverse = base_of(*table.find("PI_XBTUSD"));
    const auto& linear = base_of(*table.find("PF_XBTUSD"));
    EXPECT_EQ(inverse.local_symbol.view(), "BTC/USD-INV");
    EXPECT_EQ(linear.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(inverse.kind, ContractKind::Inverse);
    EXPECT_EQ(asset_key(inverse), asset_key(linear));
    EXPECT_EQ(market_key(linear), "crypto:BTC/USD");
    EXPECT_EQ(market_key(inverse), "crypto:BTC/USD-INV");

    std::vector<std::string> locals;
    for (const auto& listed : table.all()) {
        locals.emplace_back(base_of(listed).local_symbol.view());
    }
    std::ranges::sort(locals);
    EXPECT_EQ(std::ranges::adjacent_find(locals), locals.end());
}

} // namespace
} // namespace raikiri::venues::kraken
