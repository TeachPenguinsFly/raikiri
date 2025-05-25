#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/rest.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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

// The catalogue every mapping test resolves product ids against, built the
// way the adapter builds it: real bytes -> parse -> map -> intern.
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
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Kraken, .enable_spot = false, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Kraken);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(KrakenVenue, OrderEntryRefusesPermanently) {
    // Read-only venue: an order must fail loudly, never wait for a verdict
    // that no code will ever produce.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Kraken, .enable_spot = false, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    // A pull this adapter cannot perform must SAY so. Answering with an empty
    // vector would be indistinguishable from "the venue holds nothing of ours",
    // and a reconciler reading it that way concludes we are flat when we are not.
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(exec::caps_of(VenueId::Kraken).position_pull);
}

TEST(KrakenVenue, TestnetIsRefusedRatherThanRedirectedToProduction) {
    // Kraken retired its demo host. A --testnet run that connected anyway
    // would be a production socket wearing the wrong label.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Kraken, .enable_perp = true, .testnet = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_FALSE(adapter->data->start().has_value());
}

// ------------------------------------------------------------- wire ----

TEST(KrakenWire, EventDispatchResolvesKindAndProduct) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker_pf.json", EventKind::Ticker},
                                     std::pair{"ws_trade.json", EventKind::Trade},
                                     std::pair{"ws_trade_snapshot.json", EventKind::Trade},
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
    // A refusal and an acknowledgement differ by one field value. Reading a
    // refusal as an ack turns "the venue rejected this feed" into "this feed
    // is quiet", which is indistinguishable from a market with no activity.
    const auto ack = fixture("ws_subscribed.json");
    auto event = parse_event(ack);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Subscribed);

    const auto info = fixture("ws_info.json");
    event = parse_event(info);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Info);

    // VERIFIED live: the refusal event is "alert", not the "error" the docs
    // suggest, and it carries a message with no code beside it.
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
    // `bid` as a string is the shape a venue drifts into; it must not read as
    // a zero touch.
    EXPECT_FALSE(
        parse_ticker(R"({"feed":"ticker","product_id":"PF_XBTUSD","bid":"1"})").has_value());
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
    // Epoch milliseconds as a JSON number, not a decimal to be scaled.
    EXPECT_EQ(wire->time_ms, 1786395177000ULL);
    EXPECT_EQ(wire->next_funding_rate_time_ms, 1786395600000ULL);
    EXPECT_FALSE(wire->suspended);
    // The token is handed over untouched; map.cpp owns every interpretation.
    EXPECT_EQ(wire->relative_funding_rate, "0.00001477885");
    EXPECT_EQ(wire->mark_price, "64113.93588534861");
}

TEST(KrakenWire, InverseTickerSpellsItsRateWithAnExponent) {
    // Same field, same socket, different spelling: PI_XBTUSD's relative rate
    // arrives in exponent form while PF_XBTUSD's is a plain decimal. A parser
    // that reads only one loses whole contracts and looks like a quiet feed.
    // The frame must outlive the parse: every view handed back points into
    // the caller's buffer, which is the whole reason the parsers are zero-copy.
    const auto frame = fixture("ws_ticker_pi.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->product_id, "PI_XBTUSD");
    EXPECT_EQ(wire->relative_funding_rate, "8.907125e-6");
    EXPECT_EQ(wire->bid, "63903.0");
}

TEST(KrakenWire, SnapshotAndLiveTradeShareOneParser) {
    // The two arrive under different feed names — subscribing yields
    // trade_snapshot first and trade afterwards — so a parser that reads only
    // the singular silently drops the first hundred prints.
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

TEST(KrakenWire, BookSnapshotIsWholeAndDeltasChainBySeq) {
    static LevelBuffer storage;
    const auto snapshot_frame = fixture("ws_book_snapshot.json");
    const auto snapshot = parse_book(snapshot_frame, storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    // Kraken pushes the WHOLE book, not a fixed depth: 1751 bids here against
    // okx's flat 400, which is why k_max_levels is 4096 rather than inherited.
    EXPECT_EQ(snapshot->bids.size(), 1751U);
    EXPECT_EQ(snapshot->asks.size(), 1360U);
    EXPECT_EQ(snapshot->bids[0].price, "64110.0");
    EXPECT_EQ(snapshot->seq, 103307647ULL);
    const auto first_seq = snapshot->seq;

    // A delta is ONE level with its own side, not an array — the opposite
    // book must stay empty or applying it would delete the far touch.
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

    // The documented shape: the code is a STRING NAME with a field suffix.
    const auto bad_symbol_body = fixture("rest_error_bad_symbol.json");
    const auto bad_symbol = parse_rest_envelope(bad_symbol_body);
    ASSERT_TRUE(bad_symbol.has_value());
    EXPECT_FALSE(bad_symbol->ok);
    EXPECT_EQ(bad_symbol->error, "invalidArgument: symbol");

    // VERIFIED live: this one answers under HTTP 200, so a status-code-only
    // check would read an auth failure as a success.
    const auto auth_body = fixture("rest_error_auth.json");
    const auto auth = parse_rest_envelope(auth_body);
    ASSERT_TRUE(auth.has_value());
    EXPECT_FALSE(auth->ok);
    EXPECT_EQ(auth->error, "authenticationError");

    // The routing-layer shape carries no `error` string at all; falling back
    // to `status` keeps it from classifying as an unnamed failure.
    const auto missing_body = fixture("rest_error_not_found.json");
    const auto missing = parse_rest_envelope(missing_body);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->ok);
    EXPECT_EQ(missing->error, "NOT_FOUND");
}

TEST(KrakenWire, ServerTimeIsIsoWithAVariableFraction) {
    // Kraken's only clock is this string, and its fractional width varies
    // within a single session — slicing it at a fixed offset silently shifts
    // the venue clock by hundreds of milliseconds.
    EXPECT_EQ(*parse_iso8601_ms("1970-01-01T00:00:00.000Z"), 0ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:52:16.918Z"), 1786395136918ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:56:08.64Z"), 1786395368640ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:52:17.246037Z"), 1786395137246ULL);
    EXPECT_EQ(*parse_iso8601_ms("2026-08-10T20:52:16Z"), 1786395136000ULL);
    EXPECT_FALSE(parse_iso8601_ms("2026-08-10").has_value());
    EXPECT_FALSE(parse_iso8601_ms("2026/08/10T20:52:16Z").has_value());
    // Epoch milliseconds must never reach this parser, nor it them.
    EXPECT_FALSE(parse_iso8601_ms("1786395136918").has_value());
}

// -------------------------------------------------------------- map ----

TEST(KrakenMap, NumberParserReadsBothSpellingsAndRefusesWhatItCannotHold) {
    EXPECT_EQ(parse_number_e9("64110.0")->raw, 64'110'000'000'000LL);
    EXPECT_EQ(parse_number_e9("0.5")->raw, 500'000'000LL);
    EXPECT_EQ(parse_number_e9("1")->raw, 1'000'000'000LL);
    EXPECT_EQ(parse_number_e9("-1.5")->raw, -1'500'000'000LL);
    // Exponent form is the same value; Kraken uses both for one field.
    EXPECT_EQ(parse_number_e9("1e-3")->raw, parse_number_e9("0.001")->raw);
    EXPECT_EQ(parse_number_e9("8.907125e-6")->raw, parse_number_e9("0.000008907125")->raw);
    EXPECT_EQ(parse_number_e9("0.000000001")->raw, 1LL);
    EXPECT_EQ(parse_number_e9("1e2")->raw, 100'000'000'000LL);
    // Below 1e-9 there is nothing to round to, so the value is refused rather
    // than flattened to zero — PF_PEPEUSD's tick is exactly this case.
    EXPECT_FALSE(parse_number_e9("0.0000000001").has_value());
    EXPECT_FALSE(parse_number_e9("64113.93588534861").has_value());
    EXPECT_FALSE(parse_number_e9("").has_value());
    EXPECT_FALSE(parse_number_e9("abc").has_value());
    EXPECT_FALSE(parse_number_e9("1.2.3").has_value());
    EXPECT_FALSE(parse_number_e9("1e").has_value());
    EXPECT_FALSE(parse_number_e9("1e400").has_value());
}

TEST(KrakenMap, NegativePrecisionIsACoarserStepNotAFinerOne) {
    // contractValueTradePrecision is an EXPONENT, and it is negated: 4 means
    // 0.0001, -2 means 100. Reading the sign the other way gives a step of
    // 0.01 where Kraken means 100 — a ten-thousand-fold error that shows up
    // as every size being refused, or worse, silently accepted.
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
    // PF_XBTUSD linear perp, PI_XBTUSD inverse perp, PF_TURBOUSD for the
    // negative precision. The other three are each refused for a different
    // reason, and a loader that accepts any of them is wrong in a way that
    // stays invisible for months.
    ASSERT_EQ(loaded.size(), 3U);
    EXPECT_EQ(loaded[0], "PF_XBTUSD");
    EXPECT_EQ(loaded[1], "PI_XBTUSD");
    EXPECT_EQ(loaded[2], "PF_TURBOUSD");

    for (const auto& row : *wire) {
        if (row.symbol == "FI_XBTUSD_260925") {
            // Dated, and `type` does not say so: futures_inverse covers both
            // PI_ and FI_. lastTradingTime is the field that separates them.
            EXPECT_FALSE(row.last_trading_time.empty());
        } else if (row.symbol == "PF_PEPEUSD") {
            EXPECT_EQ(row.tick_size, "0.0000000001"); // finer than FixedE9 holds
        } else if (row.symbol == "PF_SOLUSD") {
            EXPECT_FALSE(row.tradeable);
        }
    }
    // Spot is a separate Kraken API surface entirely; this adapter declines it
    // rather than answering with futures rows.
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
    // Same underlying, same venue, same market type — and different ticks and
    // different quantity units. Merging them would price BTC quantities
    // against a book quoted in USD contracts.
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
    // relative_funding_rate, NOT funding_rate: the absolute field is quote
    // currency per contract on PF_ and base currency per USD on PI_, so only
    // the relative one is comparable to what other venues call a rate.
    EXPECT_EQ(funding->rate.raw, 14778); // 0.00001477885 truncated at 1e-9
    EXPECT_EQ(funding->next_funding_time.ns, 1786395600000ULL * 1'000'000ULL);
    // markPrice is an index-derived continuous value against a tick of 1, so
    // it does not divide and is left undefined rather than rounded onto a
    // price that never traded.
    EXPECT_TRUE(funding->mark_price.is_undef());
    // Kraken publishes the next settlement instant but never the period, so
    // guessing 8h here would be wrong invisibly.
    EXPECT_EQ(funding->interval_s, 0U);
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
    EXPECT_EQ(funding->rate.raw, 8907); // 8.907125e-6, exponent spelling
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
    // Kraken states the taker's direction directly; the binance inversion is
    // a binance fact, and applying it here reverses every signed flow series.
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

    // An unknown product is not an error to report, just nothing to publish.
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
    // A suspended market's last quote is not tradeable, and a strategy gating
    // on BBO must not read a stale touch as a live one.
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    // Kraken spells an empty side as a literal zero, and zero divides by any
    // tick — so without this check a missing bid publishes as a price of 0.
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
    // A zero propagating into latency histograms poisons them.
    EXPECT_EQ(ms_to_unix(0, UnixNanos{42}).ns, 42ULL);
    EXPECT_EQ(ms_to_unix(1786395177000ULL, UnixNanos{42}).ns, 1786395177000ULL * 1'000'000ULL);
}

// ------------------------------------------------------------- book ----

// The parsed views point into the frame text and into the level buffer, so
// both have to outlive the wire struct that came out of them.
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

// Bids descend, asks ascend, the two never cross and every quantity is real.
// A book that fails this is not a book, and publishing it would price every
// strategy reading shared memory.
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
    // One delta is one level. A book built from it would be a two-level book
    // presented to shm as the whole market.
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
    // Kraken pushes the whole book, so what shm takes is a cut of something
    // far deeper — the held book keeps every level the deltas may name.
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(book.bids().size(), 1751U);
    EXPECT_EQ(book.asks().size(), 1360U);
    EXPECT_EQ(out->venue_seq, d2.seq) << "seq is the forensic handle";
    EXPECT_EQ(out->ts_event.ns, 7ULL);
    EXPECT_EQ(out->ts_init.ns, 9ULL);

    // Both deltas landed, each as an absolute size at its price: 64133.0 at
    // 2.8971 and 64100.0 at 0.2475, over a 1 tick and a 0.0001 step.
    const auto at_ask = std::ranges::find_if(
        book.asks(), [](const Book::Level& level) { return level.price.raw == 64'133; });
    ASSERT_NE(at_ask, book.asks().end());
    EXPECT_EQ(at_ask->qty.raw, 28'971);
    const auto at_bid = std::ranges::find_if(
        book.bids(), [](const Book::Level& level) { return level.price.raw == 64'100; });
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

    // Applying it twice would double a size that is an absolute, not a change,
    // but dropping the book over it would cost a resync for nothing.
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
    // The rule that separates a delta venue from a snapshot venue. Dropping
    // one level here would leave a book that disagrees with Kraken's forever,
    // with nothing downstream able to see the difference.
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

    // And a size finer than the 0.0001 step is refused the same way.
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

// ----------------------------------------------------------- errors ----

TEST(KrakenErrors, ClassificationReadsTheNameNotTheWholeString) {
    // The name carries a ": field" suffix live, so matching the whole string
    // would put every real refusal in the default bucket.
    EXPECT_EQ(classify("invalidArgument: symbol"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("authenticationError"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify("apiLimitExceeded"), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify("marketUnavailable"), exec::ErrorClass::Retryable);
    // Clock/sequence skew, not bad credentials: the same payload re-signed
    // succeeds, and halting the venue would be the wrong response.
    EXPECT_EQ(classify("nonceBelowThreshold"), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify("NOT_FOUND"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("SERVICE_UNAVAILABLE"), exec::ErrorClass::Retryable);
    // An unknown refusal repeated at full speed is how an account earns an IP
    // ban; a spuriously terminal order is recoverable by reconciliation.
    EXPECT_EQ(classify("somethingKrakenAddsLater"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(""), exec::ErrorClass::Terminal);
    EXPECT_TRUE(is_benign_cancel_failure("orderForEditNotFound"));
    EXPECT_FALSE(is_benign_cancel_failure("invalidArgument: orderId"));
}

// --------------------------------------------------------------- ws ----

TEST(KrakenWs, ThreeKindsResolveToTheOneTickerFeed) {
    // Kraken bundles touch, mark and funding into one frame, so these three
    // are a single subscription and the dedupe upstream is what keeps them
    // from being sent three times.
    EXPECT_EQ(feed_for(exec::SubKind::Bbo), "ticker");
    EXPECT_EQ(feed_for(exec::SubKind::Funding), "ticker");
    EXPECT_EQ(feed_for(exec::SubKind::MarkPrice), "ticker");
    EXPECT_EQ(feed_for(exec::SubKind::Trades), "trade");
    EXPECT_EQ(feed_for(exec::SubKind::Depth), "book");
    EXPECT_TRUE(feed_for(exec::SubKind::Kline).empty());
}

TEST(KrakenWs, SubscribeFrameNamesOneFeedAndManyProducts) {
    Protocol protocol;
    const std::array<Subscription, 3> mixed{
        Subscription{.feed = "ticker", .product_id = "PF_XBTUSD"},
        Subscription{.feed = "ticker", .product_id = "PI_XBTUSD"},
        Subscription{.feed = "trade", .product_id = "PF_XBTUSD"}};

    // The grouping is the opposite of okx's: one feed, an array of products.
    // A frame therefore stops at the first entry with a different feed, and
    // the caller resumes from `consumed` rather than reordering its set.
    const auto first = protocol.subscribe_frame(mixed);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->consumed, 2U);
    EXPECT_EQ(first->text,
              R"({"event":"subscribe","feed":"ticker","product_ids":["PF_XBTUSD","PI_XBTUSD"]})");

    const auto second =
        protocol.subscribe_frame(std::span<const Subscription>{mixed}.subspan(first->consumed));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->consumed, 1U);
    EXPECT_EQ(second->text, R"({"event":"subscribe","feed":"trade","product_ids":["PF_XBTUSD"]})");

    const auto off = protocol.unsubscribe_frame(std::span<const Subscription>{mixed}.subspan(2));
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"event":"unsubscribe","feed":"trade","product_ids":["PF_XBTUSD"]})");

    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
    // An empty feed means feed_for() declined the kind; sending it would
    // subscribe to the literal "" and earn an alert instead of a stream.
    const std::array<Subscription, 1> declined{Subscription{.feed = "", .product_id = "PF_XBTUSD"}};
    EXPECT_FALSE(protocol.subscribe_frame(declined).has_value());
}

// ------------------------------------------------------------- rest ----

TEST(KrakenRest, TargetsMatchTheEndpointsTheFixturesCameFrom) {
    RequestBuilder builder;
    EXPECT_EQ(rest_host(), "futures.kraken.com");
    EXPECT_EQ(builder.instruments().target, "/derivatives/api/v3/instruments");
    // The symbol is a PATH segment on tickers and a QUERY parameter on
    // orderbook; swapping them yields a 404 and a NOT_FOUND body.
    EXPECT_EQ(builder.ticker("PF_XBTUSD").target, "/derivatives/api/v3/tickers/PF_XBTUSD");
    EXPECT_EQ(builder.orderbook("PI_XBTUSD").target,
              "/derivatives/api/v3/orderbook?symbol=PI_XBTUSD");
    EXPECT_EQ(builder.instruments().method, net::HttpMethod::Get);
}

} // namespace
} // namespace raikiri::venues::kraken
