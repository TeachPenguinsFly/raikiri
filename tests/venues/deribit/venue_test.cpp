#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/wire.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace raikiri::venues::deribit {
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

// Loads the catalogue fixture for one market. Returned by value because every
// map_* test wants its own table and the parse is cheap.
std::size_t
load_catalogue(exec::InstrumentTable& table, MarketType market, const std::string& body) {
    auto storage = std::make_unique<InstrumentBuffer>();
    const auto wire = parse_instruments(body, *storage);
    if (!wire.has_value()) {
        return 0;
    }
    std::size_t added = 0;
    for (const auto& row : *wire) {
        const auto instrument = map_instrument(row, market);
        if (!instrument.has_value()) {
            continue;
        }
        if (table.add(*instrument).has_value()) {
            ++added;
        }
    }
    return added;
}

// ------------------------------------------------------------ adapter ----

TEST(DeribitVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Deribit, .enable_spot = false, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Deribit);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(DeribitVenue, OrderEntryRefusesPermanently) {
    // Read-only venue: an order must fail loudly, never wait for a verdict
    // that no code will ever produce.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Deribit, .enable_spot = false, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    // Reconciliation pulls answer empty rather than failing: there is nothing
    // of ours here, which is different from being unable to ask.
    EXPECT_TRUE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
}

// ------------------------------------------------------------- number ----

TEST(DeribitNumber, ReadsEveryFormDeribitEmits) {
    // All four spellings are live captures from one venue on one day, which
    // is why parse_decimal_e9 alone cannot read a Deribit catalogue row.
    EXPECT_EQ(parse_number_e9("0.5")->raw, 500'000'000);
    EXPECT_EQ(parse_number_e9("10")->raw, 10 * k_e9);        // contract_size, ADA row
    EXPECT_EQ(parse_number_e9("10.0")->raw, 10 * k_e9);      // contract_size, BTC row
    EXPECT_EQ(parse_number_e9("1.0e-5")->raw, 10'000);       // tick_size, ADA row
    EXPECT_EQ(parse_number_e9("6.4e4")->raw, 64'000 * k_e9); // strike
    EXPECT_EQ(parse_number_e9("5.066e-5")->raw, 50'660);     // current_funding
    EXPECT_EQ(parse_number_e9("2.7e4")->raw, 27'000 * k_e9); // book size
    EXPECT_EQ(parse_number_e9("-0.0001")->raw, -100'000);    // maker_commission
}

TEST(DeribitNumber, RefusesWhatItCannotRepresentExactly) {
    // interest_value rides the perpetual ticker at eighteen fractional
    // digits. Refusing is the point: rounding a wire value is how a price
    // lands one tick off what the venue printed.
    EXPECT_FALSE(parse_number_e9("0.010543991548811414").has_value());
    EXPECT_FALSE(parse_number_e9("1e-10").has_value()); // below FixedE9's scale
    EXPECT_FALSE(parse_number_e9("").has_value());
    EXPECT_FALSE(parse_number_e9("\"64112.0\"").has_value()); // a quoted number
    EXPECT_FALSE(parse_number_e9("1.2.3").has_value());
    EXPECT_FALSE(parse_number_e9("1e").has_value());
    EXPECT_FALSE(parse_number_e9("NaN").has_value());
    EXPECT_FALSE(parse_number_e9("1e30").has_value()); // past int64 at 1e9 scale
}

// --------------------------------------------------------- catalogue ----

TEST(DeribitCatalogue, PerpetualArrivesUnderTheFutureKind) {
    const auto body = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    ASSERT_EQ(load_catalogue(table, MarketType::Perp, body), 1U);

    const auto* instrument = table.find("BTC-PERPETUAL");
    ASSERT_NE(instrument, nullptr);
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*instrument));
    const auto& base = base_of(*instrument);
    EXPECT_EQ(base.tick_size.raw, 500'000'000);  // 0.5 USD
    EXPECT_EQ(base.lot_size.raw, 10 * k_e9);     // min_trade_amount, 10 USD
    EXPECT_EQ(base.min_notional.raw, 10 * k_e9); // and it IS a USD notional here
    // INVERSE: 10 USD of face value per contract, settled in BTC. Binance
    // USD-M pins this to 1 because its qty is already base units; reading
    // Deribit's the same way is the misprice this test exists to catch.
    EXPECT_EQ(base.contract_value.raw, 10 * k_e9);
    // Continuous accrual, so there is no interval to state.
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 0U);
}

TEST(DeribitCatalogue, DatedFutureIsADifferentMarketFromThePerpetual) {
    const auto body = fixture("rest_get_instruments.json");
    exec::InstrumentTable perps;
    exec::InstrumentTable futures;
    EXPECT_EQ(load_catalogue(perps, MarketType::Perp, body), 1U);
    EXPECT_EQ(load_catalogue(futures, MarketType::Future, body), 1U);

    EXPECT_EQ(perps.find("BTC-25SEP26"), nullptr);
    EXPECT_EQ(futures.find("BTC-PERPETUAL"), nullptr);
    const auto* dated = futures.find("BTC-25SEP26");
    ASSERT_NE(dated, nullptr);
    ASSERT_TRUE(std::holds_alternative<FutureInstrument>(*dated));
    EXPECT_EQ(std::get<FutureInstrument>(*dated).expiry.ns, 1790323200000ULL * 1'000'000ULL);
    EXPECT_EQ(base_of(*dated).tick_size.raw, 2'500'000'000); // 2.5, coarser than the perp
}

TEST(DeribitCatalogue, RefusesInactiveAndLinearRows) {
    const auto body = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    // Six rows in, one perpetual out: BTC-10AUG26 is delivered
    // (is_active:false), ADA_USDC-PERPETUAL is linear, and the rest are the
    // wrong market.
    EXPECT_EQ(load_catalogue(table, MarketType::Perp, body), 1U);
    EXPECT_EQ(table.find("ADA_USDC-PERPETUAL"), nullptr);

    exec::InstrumentTable futures;
    EXPECT_EQ(load_catalogue(futures, MarketType::Future, body), 1U);
    EXPECT_EQ(futures.find("BTC-10AUG26"), nullptr);
}

TEST(DeribitCatalogue, OptionsFillTheDeclaredSeam) {
    const auto body = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    ASSERT_EQ(load_catalogue(table, MarketType::Option, body), 2U);

    const auto* call = table.find("BTC-11AUG26-64000-C");
    ASSERT_NE(call, nullptr);
    ASSERT_TRUE(std::holds_alternative<OptionInstrument>(*call));
    const auto& option = std::get<OptionInstrument>(*call);
    EXPECT_EQ(option.put_call, PutCall::Call);
    EXPECT_EQ(option.expiry.ns, 1786435200000ULL * 1'000'000ULL);

    // The premium is quoted in BTC at a 0.0001 tick and the quantity in coin
    // contracts at a 0.1 step — contract_size is 1.0, which would drop a
    // real 23.1 quote, so min_trade_amount is the step that gets stored.
    const auto& base = base_of(*call);
    EXPECT_EQ(base.tick_size.raw, 100'000);       // 0.0001 BTC
    EXPECT_EQ(base.lot_size.raw, 100'000'000);    // 0.1 coin contracts
    EXPECT_EQ(base.contract_value.raw, 1 * k_e9); // 1.0 coin per contract

    // Strike is a PriceTicks, so it is stored in the PREMIUM's tick even
    // though the strike itself is denominated in USD. The NUMBER round-trips
    // exactly; only the currency of the tick differs, and InstrumentBase has
    // no currency to record it in. Reading a strike means going back through
    // ticks_to_price, never comparing its tick count against a quote's.
    EXPECT_EQ(option.strike.raw, 640'000'000);
    EXPECT_EQ(base.ticks_to_price(option.strike).raw, 64'000 * k_e9);

    const auto* put = table.find("BTC-11AUG26-64000-P");
    ASSERT_NE(put, nullptr);
    EXPECT_EQ(std::get<OptionInstrument>(*put).put_call, PutCall::Put);
}

TEST(DeribitCatalogue, SpotIsNotModelled) {
    const auto body = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    EXPECT_EQ(load_catalogue(table, MarketType::Spot, body), 0U);
}

TEST(DeribitCatalogue, ServerTimeReadsABareNumberResult) {
    const auto time = parse_server_time(fixture("rest_get_time.json"));
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(*time, 1786395037897ULL);
}

// -------------------------------------------------------------- wire ----

TEST(DeribitWire, DispatchesOnTheChannelPrefixAndItsShape) {
    // The two book streams differ only in how many dot-separated parameters
    // the channel carries, so this is where that reading is pinned.
    const auto kind_of = [](std::string_view name) {
        const auto event = parse_event(fixture(name));
        return event.has_value() ? event->kind : EventKind::Unknown;
    };
    EXPECT_EQ(kind_of("ws_book_bbo.json"), EventKind::Bbo);
    EXPECT_EQ(kind_of("ws_book_snapshot.json"), EventKind::Book);
    EXPECT_EQ(kind_of("ws_book_change.json"), EventKind::Book);
    EXPECT_EQ(kind_of("ws_trades.json"), EventKind::Trade);
    EXPECT_EQ(kind_of("ws_ticker_perpetual.json"), EventKind::Ticker);
    EXPECT_EQ(kind_of("ws_subscribe_ack.json"), EventKind::Response);
    EXPECT_EQ(kind_of("ws_error_raw_unauthorized.json"), EventKind::Response);
    EXPECT_EQ(kind_of("ws_heartbeat_test_request.json"), EventKind::Heartbeat);

    const auto frame = fixture("ws_book_bbo.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->instrument, "BTC-PERPETUAL");
    EXPECT_FALSE(event->needs_test_reply);
}

TEST(DeribitWire, TestRequestIsTheHeartbeatThatNeedsAnswering) {
    const auto frame = fixture("ws_heartbeat_test_request.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Heartbeat);
    EXPECT_TRUE(event->needs_test_reply);
}

TEST(DeribitWire, AcceptedChannelsAreCountedBecauseRefusalsAreSilent) {
    const auto ack = fixture("ws_subscribe_ack.json");
    const auto ok = parse_response(ack);
    ASSERT_TRUE(ok.has_value());
    EXPECT_FALSE(ok->failed());
    EXPECT_EQ(ok->id, 7);
    EXPECT_EQ(ok->accepted, 5U);

    // An unknown instrument is dropped with NO error at all — the empty list
    // is the only evidence the subscription never took.
    const auto empty = fixture("ws_subscribe_ack_empty.json");
    const auto silent = parse_response(empty);
    ASSERT_TRUE(silent.has_value());
    EXPECT_FALSE(silent->failed());
    EXPECT_EQ(silent->accepted, 0U);
}

TEST(DeribitWire, ErrorIsAnObjectNotAStatusField) {
    const auto frame = fixture("ws_error_raw_unauthorized.json");
    const auto response = parse_response(frame);
    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->failed());
    EXPECT_EQ(response->error_code, k_raw_subscriptions_unauthorized);
    EXPECT_EQ(response->error_message, "raw_subscriptions_not_available_for_unauthorized");

    // REST shares the envelope, and its codes are the NEGATIVE JSON-RPC ones.
    const auto rest = parse_response(fixture("rest_error_invalid_kind.json"));
    ASSERT_TRUE(rest.has_value());
    ASSERT_TRUE(rest->failed());
    EXPECT_EQ(rest->error_code, k_invalid_params);
}

TEST(DeribitWire, IncrementalBookStatesTheActionPerLevel) {
    const auto snapshot_body = fixture("ws_book_snapshot.json");
    const auto change_body = fixture("ws_book_change.json");
    auto storage = std::make_unique<LevelBuffer>();

    const auto snapshot = parse_book(snapshot_body, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    EXPECT_EQ(snapshot->change_id, 169357537783);
    EXPECT_EQ(snapshot->prev_change_id, 0); // absent, not a sentinel
    ASSERT_FALSE(snapshot->bids.empty());
    EXPECT_EQ(snapshot->bids[0].action, LevelAction::New);
    const auto chain_from = snapshot->change_id;

    const auto change = parse_book(change_body, *storage);
    ASSERT_TRUE(change.has_value());
    EXPECT_FALSE(change->snapshot);
    EXPECT_EQ(change->prev_change_id, chain_from); // the continuity chain
    // The action is the removal, not the size. MEASURED 2026-08-17 across 904
    // deletes on three perpetuals: every one also carried 0.0, so the two
    // readings agree today — but the action is what Deribit defines, and the
    // book keys on it rather than on a coincidence.
    ASSERT_GE(change->bids.size(), 2U);
    EXPECT_EQ(change->bids[1].action, LevelAction::Delete);
    ASSERT_GE(change->asks.size(), 3U);
    EXPECT_EQ(change->asks[0].action, LevelAction::Change);
    EXPECT_EQ(change->asks[2].price, "1.641e5"); // an exponent-form price
}

// --------------------------------------------------------------- map ----

TEST(DeribitMap, BboComesFromTheDepthOneGroupedBook) {
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    ASSERT_EQ(load_catalogue(table, MarketType::Perp, catalogue), 1U);

    const auto frame = fixture("ws_book_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->instrument, "BTC-PERPETUAL");

    const auto bbo = map_bbo(*wire, table, UnixNanos{7});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 128'224); // 64112.0 / 0.5
    EXPECT_EQ(bbo->ask.raw, 128'225);
    EXPECT_EQ(bbo->bid_qty.raw, 20'141); // 201410 USD / 10 USD
    EXPECT_EQ(bbo->ask_qty.raw, 4'432);
    EXPECT_EQ(bbo->ts_event.ns, 1786395070130ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, 7U);
}

TEST(DeribitMap, OptionBboUsesTheFinerCoinStep) {
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    ASSERT_EQ(load_catalogue(table, MarketType::Option, catalogue), 2U);

    const auto frame = fixture("ws_book_bbo_option.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 40); // 0.004 BTC / 0.0001
    EXPECT_EQ(bbo->ask.raw, 48);
    // 23.1 contracts is not a multiple of contract_size (1.0); storing that
    // as the lot step would have dropped this quote entirely.
    EXPECT_EQ(bbo->bid_qty.raw, 231);
    EXPECT_EQ(bbo->ask_qty.raw, 70);
}

TEST(DeribitMap, TradeDirectionIsAlreadyTheAggressor) {
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    ASSERT_EQ(load_catalogue(table, MarketType::Perp, catalogue), 1U);

    const auto frame = fixture("ws_trades.json");
    auto storage = std::make_unique<TradeBuffer>();
    const auto wire = parse_trades(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    EXPECT_EQ((*wire)[0].trade_id, "440052883");
    EXPECT_EQ((*wire)[0].trade_seq, 296055264);

    const auto trade = map_trade((*wire)[0], table, UnixNanos{3});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 128'225);
    // 40 USD / 10 USD = 4 lots, and the frame's own `contracts` field says
    // 4.0 — the lot step and the contract size coincide on this instrument.
    EXPECT_EQ(trade->qty.raw, 4);
    EXPECT_EQ(trade->aggressor, Side::Buy);
}

TEST(DeribitMap, PerpetualFundingSurvivesAnHonestZero) {
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable table;
    ASSERT_EQ(load_catalogue(table, MarketType::Perp, catalogue), 1U);

    const auto frame = fixture("ws_ticker_perpetual.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->current_funding, "5.066e-5");
    EXPECT_EQ(wire->funding_8h, "9.71e-6"); // read, deliberately not mapped

    const auto funding = map_funding(*wire, table, UnixNanos{11});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 50'660);
    // No settlement boundary exists: funding accrues continuously and the
    // eight hours are the rate's UNIT, not a countdown.
    EXPECT_EQ(funding->next_funding_time.ns, 0U);
    EXPECT_EQ(funding->interval_s, 8U * 60U * 60U);
    // Deribit publishes no forecast, so this stays undefined rather than
    // borrowing the trailing funding_8h and calling it a prediction.
    EXPECT_TRUE(funding->predicted_rate.is_undef());
}

TEST(DeribitMap, MarkPriceIsDroppedWhenItIsNotOnTheTick) {
    // Deribit's perpetual mark is a fair value, not a book price: 64111.95
    // against a 0.5 tick. Under the no-rounding rule that is dropped, so the
    // field stays undefined rather than reporting a price the venue never
    // quoted. It DOES land for options, whose mark sits on the tick.
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable perps;
    ASSERT_EQ(load_catalogue(perps, MarketType::Perp, catalogue), 1U);
    const auto perp_frame = fixture("ws_ticker_perpetual.json");
    const auto perp_wire = parse_ticker(perp_frame);
    ASSERT_TRUE(perp_wire.has_value());
    EXPECT_EQ(perp_wire->mark_price, "64111.95");
    const auto funding = map_funding(*perp_wire, perps, UnixNanos{1});
    ASSERT_TRUE(funding.has_value());
    EXPECT_TRUE(funding->mark_price.is_undef());
}

TEST(DeribitMap, NonPerpetualTickersQuoteNoFundingAtAll) {
    const auto catalogue = fixture("rest_get_instruments.json");

    exec::InstrumentTable futures;
    ASSERT_EQ(load_catalogue(futures, MarketType::Future, catalogue), 1U);
    const auto future_frame = fixture("ws_ticker_future.json");
    const auto future_wire = parse_ticker(future_frame);
    ASSERT_TRUE(future_wire.has_value());
    EXPECT_TRUE(future_wire->current_funding.empty());
    // Absent, not zero: a dated future reported as funding at 0.0 would sit
    // in a basis series looking like a real observation.
    EXPECT_FALSE(map_funding(*future_wire, futures, UnixNanos{1}).has_value());

    exec::InstrumentTable options;
    ASSERT_EQ(load_catalogue(options, MarketType::Option, catalogue), 2U);
    const auto option_frame = fixture("ws_ticker_option.json");
    const auto option_wire = parse_ticker(option_frame);
    ASSERT_TRUE(option_wire.has_value());
    // greeks and mark_iv ride this frame and are deliberately not read.
    EXPECT_EQ(option_wire->mark_price, "0.0044");
    EXPECT_FALSE(map_funding(*option_wire, options, UnixNanos{1}).has_value());
}

TEST(DeribitMap, UnknownInstrumentIsDroppedRatherThanInvented) {
    const exec::InstrumentTable empty;
    const auto frame = fixture("ws_book_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_bbo(*wire, empty, UnixNanos{1}).has_value());
}

// ---------------------------------------------------------------- ws ----

// -------------------------------------------------------------- book ----

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
    const auto* instrument = table.find("BTC-PERPETUAL");
    EXPECT_NE(instrument, nullptr);
    const auto& base = base_of(*instrument);
    return Book{base.id, base.tick_size, base.lot_size};
}

exec::InstrumentTable perp_catalogue() {
    exec::InstrumentTable table;
    EXPECT_EQ(load_catalogue(table, MarketType::Perp, fixture("rest_get_instruments.json")), 1U);
    return table;
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

TEST(DeribitBook, ADeltaBeforeAnySnapshotPublishesNothing) {
    const auto table = perp_catalogue();
    auto book = book_for(table);
    BookFrame delta;
    EXPECT_EQ(book.apply(book_of(delta, "ws_book_change_full.json")), BookResult::Ignored);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(DeribitBook, SnapshotAndChangeMakeABookSharedMemoryCanTake) {
    const auto table = perp_catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame change_frame;

    const auto snapshot = book_of(snapshot_frame, "ws_book_snapshot_full.json");
    EXPECT_EQ(book.apply(snapshot), BookResult::Resynced);
    EXPECT_EQ(book.bids().size(), 904U);
    EXPECT_EQ(book.asks().size(), 1063U);

    const auto change = book_of(change_frame, "ws_book_change_full.json");
    EXPECT_EQ(change.prev_change_id, snapshot.change_id);
    // Deribit's ids are NOT contiguous — this pair is 398 apart — so only the
    // back-pointer decides, never arithmetic on the id itself.
    EXPECT_GT(change.change_id - snapshot.change_id, 1);
    EXPECT_EQ(book.apply(change), BookResult::Applied);

    const auto out = book.snapshot(UnixNanos{7}, UnixNanos{9});
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    // Deribit pushes the whole book, so what shm takes is a cut of something
    // far deeper.
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(out->bids[0].price.raw, 128'070); // 64035.0 over the 0.5 tick
    EXPECT_EQ(out->asks[0].price.raw, 128'071);
    EXPECT_EQ(out->bids[0].qty.raw, 8593); // 85930 USD over the 10 USD step
    EXPECT_EQ(out->venue_seq, static_cast<std::uint64_t>(change.change_id));
    EXPECT_EQ(out->ts_event.ns, 7ULL);
    EXPECT_EQ(out->ts_init.ns, 9ULL);
}

TEST(DeribitBook, DeleteRemovesTheLevelAndNewAddsOne) {
    const auto table = perp_catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame change_frame;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot_full.json")),
              BookResult::Resynced);

    const auto before = book.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->asks[16].price.raw, 128'092) << "64046.0, the level the delta deletes";

    EXPECT_EQ(book.apply(book_of(change_frame, "ws_book_change_full.json")), BookResult::Applied);
    const auto after = book.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(after.has_value());

    const auto gone =
        std::find_if(after->asks.begin(),
                     after->asks.begin() + after->ask_count,
                     [](const BookLevel& level) { return level.price.raw == 128'092; });
    EXPECT_EQ(gone, after->asks.begin() + after->ask_count) << "the delete removed it";
    // 64045.0 was in no side of the snapshot; the `new` beside the delete put
    // it there, so the side holds the same count for a different reason.
    const auto added =
        std::find_if(after->asks.begin(),
                     after->asks.begin() + after->ask_count,
                     [](const BookLevel& level) { return level.price.raw == 128'090; });
    ASSERT_NE(added, after->asks.begin() + after->ask_count);
    EXPECT_EQ(added->qty.raw, 1300); // 13000 USD over the 10 USD step
    EXPECT_EQ(book.asks().size(), 1063U);
    EXPECT_EQ(book.bids().size(), 905U); // three new, two deleted
}

TEST(DeribitBook, ABrokenBackPointerDropsTheBookUntilTheNextSnapshot) {
    const auto table = perp_catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame change_frame;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot_full.json")),
              BookResult::Resynced);

    auto orphan = book_of(change_frame, "ws_book_change_full.json");
    orphan.prev_change_id += 1; // a frame we never saw sits between them

    EXPECT_EQ(book.apply(orphan), BookResult::GapDetected);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(DeribitBook, ARedeliveredFrameIsIgnoredRatherThanTreatedAsAGap) {
    const auto table = perp_catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame change_frame;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot_full.json")),
              BookResult::Resynced);
    const auto change = book_of(change_frame, "ws_book_change_full.json");
    EXPECT_EQ(book.apply(change), BookResult::Applied);

    EXPECT_EQ(book.apply(change), BookResult::Ignored);
    EXPECT_EQ(book.gaps(), 0U);
    EXPECT_TRUE(book.synced());
    EXPECT_EQ(book.last_change_id(), change.change_id);
}

TEST(DeribitBook, ALevelOffTheStepInvalidatesTheWholeBook) {
    // The rule that separates a delta venue from a snapshot venue. Dropping
    // one level here would leave a book that disagrees with Deribit's forever,
    // with nothing downstream able to see the difference.
    const auto table = perp_catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    const auto snapshot = book_of(snapshot_frame, "ws_book_snapshot_full.json");
    EXPECT_EQ(book.apply(snapshot), BookResult::Resynced);

    const std::array<WireLevel, 1> bad{
        WireLevel{.action = LevelAction::Change, .price = "64035.25", .qty = "1.0e3"}};
    WireBook off_tick{};
    off_tick.instrument = "BTC-PERPETUAL";
    off_tick.change_id = snapshot.change_id + 1;
    off_tick.prev_change_id = snapshot.change_id;
    off_tick.bids = bad;
    EXPECT_EQ(book.apply(off_tick), BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());

    // A size finer than the 10 USD contract is refused the same way, and a
    // delete is not exempt: its price still has to name a level we hold.
    auto again = book_for(table);
    EXPECT_EQ(again.apply(snapshot), BookResult::Resynced);
    const std::array<WireLevel, 1> sub_lot{
        WireLevel{.action = LevelAction::Delete, .price = "64035.25", .qty = "0.0"}};
    WireBook fine{};
    fine.instrument = "BTC-PERPETUAL";
    fine.change_id = snapshot.change_id + 1;
    fine.prev_change_id = snapshot.change_id;
    fine.asks = sub_lot;
    EXPECT_EQ(again.apply(fine), BookResult::BadLevel);
    EXPECT_FALSE(again.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(DeribitWs, ChannelNamesCarryTheInstrumentAndTheParameters) {
    std::array<char, k_max_channel_len> buffer{};
    EXPECT_EQ(channel_for(exec::SubKind::Bbo, "BTC-PERPETUAL", buffer),
              "book.BTC-PERPETUAL.none.1.100ms");
    EXPECT_EQ(channel_for(exec::SubKind::Trades, "BTC-PERPETUAL", buffer),
              "trades.BTC-PERPETUAL.100ms");
    EXPECT_EQ(channel_for(exec::SubKind::Depth, "BTC-PERPETUAL", buffer),
              "book.BTC-PERPETUAL.100ms");
    // Funding and mark are one frame here, so both kinds name one channel and
    // the venue's desired set collapses them.
    EXPECT_EQ(channel_for(exec::SubKind::Funding, "BTC-PERPETUAL", buffer),
              "ticker.BTC-PERPETUAL.100ms");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice, "BTC-PERPETUAL", buffer),
              "ticker.BTC-PERPETUAL.100ms");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline, "BTC-PERPETUAL", buffer).empty());
    EXPECT_TRUE(channel_for(exec::SubKind::Bbo, "", buffer).empty());

    // A buffer that cannot hold the whole channel yields empty rather than a
    // prefix, because a truncated channel names a different instrument.
    std::array<char, 8> tiny{};
    EXPECT_TRUE(channel_for(exec::SubKind::Bbo, "BTC-PERPETUAL", tiny).empty());
}

TEST(DeribitWs, SubscribeFramesAreJsonRpcAndDeliberatelySmall) {
    Protocol protocol;
    const std::array<Subscription, 2> two{
        Subscription{.channel = "book.BTC-PERPETUAL.none.1.100ms"},
        Subscription{.channel = "trades.BTC-PERPETUAL.100ms"}};
    const auto frame = protocol.subscribe_frame(two);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(frame->text,
              R"({"jsonrpc":"2.0","id":1,"method":"public/subscribe","params":{"channels":)"
              R"(["book.BTC-PERPETUAL.none.1.100ms","trades.BTC-PERPETUAL.100ms"]}})");

    // Ids advance so a reply can be attributed to the batch that asked.
    const auto next = protocol.subscribe_frame(two);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->id, 2);

    // One refused channel fails the WHOLE batch on this venue (verified live
    // with 13778), so an unbuildable channel must never reach the wire beside
    // good ones.
    const std::array<Subscription, 2> poisoned{
        Subscription{.channel = "trades.BTC-PERPETUAL.100ms"}, Subscription{.channel = ""}};
    EXPECT_FALSE(protocol.subscribe_frame(poisoned).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());

    // The batch ceiling is what bounds that blast radius.
    std::array<Subscription, k_max_subscriptions_per_frame + 4> many{};
    for (auto& entry : many) {
        entry.channel = "ticker.BTC-PERPETUAL.100ms";
    }
    const auto capped = protocol.subscribe_frame(many);
    ASSERT_TRUE(capped.has_value());
    EXPECT_EQ(capped->consumed, k_max_subscriptions_per_frame);
}

TEST(DeribitWs, HeartbeatFramesMatchWhatDeribitAnswered) {
    Protocol protocol;
    EXPECT_EQ(
        protocol.set_heartbeat_frame(15).text,
        R"({"jsonrpc":"2.0","id":1,"method":"public/set_heartbeat","params":{"interval":15}})");
    EXPECT_EQ(protocol.test_frame().text,
              R"({"jsonrpc":"2.0","id":2,"method":"public/test","params":{}})");
}

TEST(DeribitWs, TestnetIsASeparateHost) {
    EXPECT_EQ(stream_host(false), "www.deribit.com");
    EXPECT_EQ(stream_host(true), "test.deribit.com");
    EXPECT_EQ(stream_path(), "/ws/api/v2");
}

// -------------------------------------------------------------- rest ----

TEST(DeribitRest, TargetsNameTheMethodInThePath) {
    RequestBuilder builder;
    EXPECT_EQ(builder.server_time().target, "/api/v2/public/get_time");
    // "perpetual" is not a kind: asking for one is Invalid params (verified),
    // so the perpetuals ride the future call.
    EXPECT_EQ(builder.instruments("BTC", "future").target,
              "/api/v2/public/get_instruments?currency=BTC&kind=future");
    EXPECT_EQ(builder.instruments("ETH", "option").target,
              "/api/v2/public/get_instruments?currency=ETH&kind=option");
    EXPECT_EQ(rest_host(false), "www.deribit.com");
    EXPECT_EQ(rest_host(true), "test.deribit.com");
}

// ------------------------------------------------------------ errors ----

TEST(DeribitErrors, ClassificationKeepsRecoverableThingsRecoverable) {
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_too_many_concurrent_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_retry), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_settlement_in_progress), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_temporarily_unavailable), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_system_maintenance), exec::ErrorClass::Retryable);
    // The session lapsed, not the credentials — logging in again is the fix,
    // and halting the venue over it would be the wrong response.
    EXPECT_EQ(classify(k_authorization_required), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_subscribe_error_unsubscribed), exec::ErrorClass::Retryable);

    EXPECT_EQ(classify(k_invalid_credentials), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_unauthorized), exec::ErrorClass::AuthFailure);

    // Both code spaces default to Terminal, including the negative JSON-RPC
    // ones: an unknown refusal repeated at full speed earns an IP ban.
    EXPECT_EQ(classify(k_invalid_params), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_method_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_raw_subscriptions_unauthorized), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
}

} // namespace
} // namespace raikiri::venues::deribit
