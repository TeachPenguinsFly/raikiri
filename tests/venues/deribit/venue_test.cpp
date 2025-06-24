#include <raikiri/common/int128.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/sign.hpp>
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

// Deribit's own worked example from the authentication docs, kept verbatim so
// a change to the string layout fails here rather than as 13004 in production.
constexpr std::string_view k_doc_client_id = "AMANDA";
constexpr std::string_view k_doc_secret = "AMANDASECRECT";

constexpr std::string_view k_key = "deribit-test-key";
constexpr std::string_view k_secret = "deribit-test-secret";

void load_credentials(net::Signer& signer) {
    signer.set_api_key(k_key);
    ASSERT_TRUE(signer.secret().assign(k_secret).has_value());
}

std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

// ------------------------------------------------------------ adapter ----

TEST(DeribitVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Deribit, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Deribit);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(DeribitVenue, EveryMarketItServesCanActuallyBeBuilt) {
    // The order path has refused the right markets since the beginning, but
    // no PROCESS could ask for a dated future or an option: VenueConfig
    // carried two booleans, spot and perp, and nothing else was sayable. This
    // is the seam finally being reachable from a gateway.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    for (const auto market :
         {MarketType::Perp, MarketType::Future, MarketType::Option}) {
        const auto adapter =
            exec::make_venue({.venue = VenueId::Deribit, .market = market}, sink, clock);
        ASSERT_TRUE(adapter.has_value()) << to_string(market);
        EXPECT_NE(adapter->instruments, nullptr) << to_string(market);
    }
    const auto spot =
        exec::make_venue({.venue = VenueId::Deribit, .market = MarketType::Spot}, sink, clock);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error(), exec::MakeError::UnsupportedMarket);
}

TEST(DeribitVenue, ServesEveryMarketItListsExceptSpot) {
    // The refusal is decided BEFORE the instrument table is consulted, so an
    // order for a market this adapter never loaded says so instead of blaming
    // a stale catalogue for an instrument that was never in it.
    EXPECT_FALSE(market_refusal(MarketType::Perp).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Future).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Option).has_value());
    EXPECT_EQ(market_refusal(MarketType::Spot), BuildError::SpotNotModelled);
}

TEST(DeribitVenue, OrderEntryWithoutCredentialsFailsAtTheConfigNotTheVenue) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Deribit, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // AuthFailure, not Terminal: the first sends an operator to the config,
    // the second to a venue status page.
    const auto started = adapter->execution->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().cls, exec::ErrorClass::AuthFailure);
    EXPECT_FALSE(adapter->execution->connected());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{7});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    // An order that cannot even be built must be answered, not dropped: a
    // strategy waiting on a verdict no code will produce waits forever.
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);

    sink.events.clear();
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::CancelRejected);

    sink.events.clear();
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ModifyRejected);
}

TEST(DeribitVenue, SpotIsRefusedByNameOnEveryEntryPoint) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Deribit, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // Never an empty vector: a reconciler cannot tell "we serve no spot here"
    // from "the account holds no spot", and reads the second as the first.
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Spot).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Spot, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Spot).has_value());
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
    EXPECT_EQ(base.kind, ContractKind::Inverse); // settles in BTC, priced in USD
    // ONE, not the venue's contract_size of 10. portfolio::settlement_value
    // computes qty x lot_size x contract_value, and lot_size is already the 10
    // USD block Deribit sizes an order in — so storing 10 here would price a
    // one-lot position at 100 USD. DeribitPnl below is where that is checked
    // against the arithmetic rather than against this assertion.
    EXPECT_EQ(base.contract_value.raw, k_e9);
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
    EXPECT_EQ(option_wire->mark_price, "0.0044");
    // An option pays no funding, and the same frame is where its greeks are.
    EXPECT_FALSE(map_funding(*option_wire, options, UnixNanos{1}).has_value());
    EXPECT_TRUE(map_greeks(*option_wire, options, UnixNanos{1}).has_value());
}

TEST(DeribitMap, TheVenuesGreeksRideTheTickerAndAreTakenAsGiven) {
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable options;
    ASSERT_EQ(load_catalogue(options, MarketType::Option, catalogue), 2U);

    const auto frame = fixture("ws_ticker_option.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    ASSERT_TRUE(has_greeks(*wire));

    const auto greeks = map_greeks(*wire, options, UnixNanos{1});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument.market(), MarketType::Option);
    EXPECT_EQ(greeks->delta.raw, 575'880'000);      // 0.57588
    EXPECT_EQ(greeks->gamma.raw, 680'000);          // 0.00068
    EXPECT_EQ(greeks->vega.raw, 8'918'550'000);     // 8.91855
    EXPECT_EQ(greeks->theta.raw, -112'532'180'000); // negative, and stays so
    EXPECT_EQ(greeks->rho.raw, 462'080'000);

    // PERCENT ON THE WIRE, RATIO IN THE DOMAIN. 25.24 % is 0.2524, and a
    // percent that slipped through unconverted would be a 100x vol.
    EXPECT_EQ(greeks->mark_iv.raw, 252'400'000);
    EXPECT_EQ(greeks->bid_iv.raw, 218'400'000);
    EXPECT_EQ(greeks->ask_iv.raw, 275'900'000);
    EXPECT_LT(greeks->bid_iv.raw, greeks->ask_iv.raw);

    // The underlying is a dollar price and stays one: dividing it by the
    // option's tick — the premium's step, in BTC — would mean nothing.
    EXPECT_EQ(greeks->underlying_price.raw, 64'107'290'000'000);
}

TEST(DeribitMap, APerpetualTickerCarriesNoGreeksAtAll) {
    // The separation that matters: an option whose greeks are genuinely zero
    // — a deep in-the-money call has gamma 0 — must not read the same as an
    // instrument that has none.
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable perps;
    ASSERT_GT(load_catalogue(perps, MarketType::Perp, catalogue), 0U);

    const auto frame = fixture("ws_ticker_perpetual.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(has_greeks(*wire));
    EXPECT_FALSE(map_greeks(*wire, perps, UnixNanos{1}).has_value());
    // And the perpetual's own row still maps, from the same parser.
    EXPECT_TRUE(map_funding(*wire, perps, UnixNanos{1}).has_value());
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

TEST(DeribitWs, ThePrivateSocketAuthenticatesTheConnectionNotEachMessage) {
    net::Signer signer;
    signer.set_api_key(k_key);
    ASSERT_TRUE(signer.secret().assign(k_secret).has_value());
    Protocol protocol;

    const auto auth = protocol.auth_frame(signer, UnixNanos{1'787'751'010'667'000'000ULL});
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->text,
              R"({"jsonrpc":"2.0","id":1,"method":"public/auth","params":)"
              R"({"grant_type":"client_signature","client_id":"deribit-test-key",)"
              R"("timestamp":1787751010667,"nonce":"n1","data":"","signature":)"
              R"("5bd5b5b9687db959e05367beefecb21ee3259a000a5c86d36634cc42d3f361e5"}})");
    // The reply is a plain JSON-RPC response; the socket then carries private
    // calls with no token on any of them.
    EXPECT_EQ(auth->consumed, 0U);

    std::array<char, k_max_channel_len> buffer{};
    EXPECT_EQ(user_changes_channel("future", "BTC", buffer), "user.changes.future.BTC.raw");
    EXPECT_EQ(user_changes_channel("option", "ETH", buffer), "user.changes.option.ETH.raw");
    EXPECT_TRUE(user_changes_channel("future", {}, buffer).empty());

    const std::array<Subscription, 2> channels{
        Subscription{.channel = "user.changes.future.BTC.raw"},
        Subscription{.channel = "user.changes.option.BTC.raw"}};
    const auto subscribe = protocol.private_subscribe_frame(channels);
    ASSERT_TRUE(subscribe.has_value());
    EXPECT_EQ(subscribe->text,
              R"({"jsonrpc":"2.0","id":2,"method":"private/subscribe","params":{"channels":)"
              R"(["user.changes.future.BTC.raw","user.changes.option.BTC.raw"]}})");
    EXPECT_EQ(subscribe->consumed, 2U);

    // Without credentials there is nothing to send, and saying so beats an
    // auth frame carrying an empty signature the venue answers 13004 to.
    Protocol bare_protocol;
    const net::Signer bare;
    EXPECT_FALSE(bare_protocol.auth_frame(bare, UnixNanos{1}).has_value());
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


// ------------------------------------------------------ order entry ----

// The perpetual, the dated future and one option side by side — the whole
// point being that order entry treats all three the same way.
exec::InstrumentTable full_catalogue(const std::string& body) {
    exec::InstrumentTable table;
    (void)load_catalogue(table, MarketType::Perp, body);
    (void)load_catalogue(table, MarketType::Future, body);
    (void)load_catalogue(table, MarketType::Option, body);
    return table;
}

OrderCmd order_for(const exec::InstrumentTable& table, std::string_view symbol) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01K3QW9F2H6M5N8P0R2T4V6X8Z");
    if (const auto* instrument = table.find(symbol); instrument != nullptr) {
        cmd.instrument = base_of(*instrument).id;
    }
    return cmd;
}

TEST(DeribitSign, MatchesTheVendorsOwnWorkedExample) {
    // Timestamp, nonce and expected digest are all copied out of Deribit's
    // client_signature documentation. Both newlines are load-bearing: dropping
    // the one after an empty `data` yields a digest the venue answers 13004
    // to, which reads exactly like a wrong secret.
    std::array<char, 128> buffer{};
    const auto message = sign::message(1'576'074'319'000ULL, "1iqt2wls", {}, buffer);
    EXPECT_EQ(message, "1576074319000\n1iqt2wls\n");

    net::Signer signer;
    signer.set_api_key(k_doc_client_id);
    ASSERT_TRUE(signer.secret().assign(k_doc_secret).has_value());
    std::array<char, net::k_hex_bytes> digest{};
    EXPECT_EQ(signer.sign(message, net::Signer::Encoding::Hex, digest),
              "56590594f97921b09b18f166befe0d1319b198bbcdad7ca73382de2f88fe9aa1");
}

TEST(DeribitSign, RestSignsMethodUriAndAnEmptyBody) {
    std::array<char, 256> buffer{};
    // The trailing newline after the body is present even though a GET has no
    // body — Deribit's own reference client emits it.
    EXPECT_EQ(sign::request_data(net::HttpMethod::Get, "/api/v2/private/get_positions", {}, buffer),
              "GET\n/api/v2/private/get_positions\n\n");
}

TEST(DeribitSignedRest, CreateOrderCarriesTheDeriHmacHeaderOverTheWholeTarget) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = order_for(table, "BTC-PERPETUAL");
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::PostOnly;
    cmd.qty = QtyLots::from_raw(20);       // 20 x 10 USD = 200 USD
    cmd.price = PriceTicks::from_raw(127'000); // 127000 x 0.5 = 63500 USD

    const auto request = builder.create_order(cmd, table, UnixNanos{1'787'751'010'667'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_EQ(request->target,
              "/api/v2/private/buy?instrument_name=BTC-PERPETUAL&amount=200"
              "&label=01K3QW9F2H6M5N8P0R2T4V6X8Z&type=limit&price=63500"
              "&time_in_force=good_til_cancelled&post_only=true&reject_post_only=true");
    EXPECT_EQ(builder.last_sign_message(),
              std::string{"1787751010667\nn1\nGET\n"} + std::string{request->target} + "\n\n");
    // Digest computed independently with `openssl dgst -sha256 -hmac` over the
    // same bytes, so this pins the whole chain and not just its last step.
    EXPECT_EQ(header_of(*request, "Authorization"),
              "deri-hmac-sha256 id=deribit-test-key,ts=1787751010667,nonce=n1,"
              "sig=66b6e110bf65302142699c7df7cc37f197acb08b6591abf1e154a0e123b99210");
}

TEST(DeribitSignedRest, PostOnlyIsAlwaysStatedBecauseTheVenueDefaultsItToTrue) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    // A market order that inherited Deribit's post_only default would be
    // refused outright, so the flag is spelled out on every order.
    auto market = order_for(table, "BTC-PERPETUAL");
    market.side = Side::Sell;
    market.order_type = OrderType::Market;
    market.qty = QtyLots::from_raw(3);
    const auto sell = builder.create_order(market, table, now);
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->target,
              "/api/v2/private/sell?instrument_name=BTC-PERPETUAL&amount=30"
              "&label=01K3QW9F2H6M5N8P0R2T4V6X8Z&type=market&post_only=false");

    // A post-only order that would cross is REJECTED rather than repriced:
    // Deribit's own post-only moves the price to just inside the spread, and
    // an order resting where the strategy never asked is worse than none.
    auto maker = order_for(table, "BTC-PERPETUAL");
    maker.flags = OrderFlags::PostOnly | OrderFlags::ReduceOnly;
    maker.qty = QtyLots::from_raw(1);
    maker.price = PriceTicks::from_raw(120'000);
    const auto reduce = builder.create_order(maker, table, now);
    ASSERT_TRUE(reduce.has_value());
    EXPECT_NE(std::string{reduce->target}.find("&post_only=true&reject_post_only=true"),
              std::string::npos);
    EXPECT_NE(std::string{reduce->target}.find("&reduce_only=true"), std::string::npos);
}

TEST(DeribitSignedRest, AnOptionOrderIsTheSameCallWithAnotherInstrumentName) {
    // The finding this test exists for: private/buy takes ONE shape, and the
    // unit of `amount` is decided by the instrument, not by a branch. The
    // perpetual's amount is USD; the option's is coin. Both come out of
    // lot_size, so neither path can borrow the other's arithmetic.
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    auto call = order_for(table, "BTC-11AUG26-64000-C");
    ASSERT_EQ(call.instrument.market(), MarketType::Option);
    call.side = Side::Sell;
    call.tif = TimeInForce::Ioc;
    call.qty = QtyLots::from_raw(15);        // 15 x 0.1 = 1.5 coin
    call.price = PriceTicks::from_raw(410);  // 410 x 0.0001 = 0.041 BTC premium
    const auto option = builder.create_order(call, table, now);
    ASSERT_TRUE(option.has_value());
    EXPECT_EQ(option->target,
              "/api/v2/private/sell?instrument_name=BTC-11AUG26-64000-C&amount=1.5"
              "&label=01K3QW9F2H6M5N8P0R2T4V6X8Z&type=limit&price=0.041"
              "&time_in_force=immediate_or_cancel&post_only=false");

    auto dated = order_for(table, "BTC-25SEP26");
    ASSERT_EQ(dated.instrument.market(), MarketType::Future);
    dated.qty = QtyLots::from_raw(5);
    dated.price = PriceTicks::from_raw(26'000); // 26000 x 2.5 = 65000 USD
    const auto future = builder.create_order(dated, table, now);
    ASSERT_TRUE(future.has_value());
    EXPECT_EQ(future->target,
              "/api/v2/private/buy?instrument_name=BTC-25SEP26&amount=50"
              "&label=01K3QW9F2H6M5N8P0R2T4V6X8Z&type=limit&price=65000"
              "&time_in_force=good_til_cancelled&post_only=false");
}

TEST(DeribitSignedRest, QuantityAndPriceMustLandOnTheInstrumentsOwnSteps) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    auto cmd = order_for(table, "BTC-PERPETUAL");
    cmd.qty = QtyLots::from_raw(0);
    cmd.price = PriceTicks::from_raw(127'000);
    EXPECT_EQ(builder.create_order(cmd, table, now).error(), BuildError::BadQuantity);

    cmd.qty = QtyLots::from_raw(20);
    cmd.price = PriceTicks::from_raw(0);
    EXPECT_EQ(builder.create_order(cmd, table, now).error(), BuildError::BadPrice);

    // An instrument for a market this table never loaded.
    cmd.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{999});
    cmd.price = PriceTicks::from_raw(127'000);
    EXPECT_EQ(builder.create_order(cmd, table, now).error(), BuildError::UnknownInstrument);

    // ...and spot, which is refused before the table is read at all.
    cmd.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Spot, SymbolId{0});
    EXPECT_EQ(builder.create_order(cmd, table, now).error(), BuildError::SpotNotModelled);
}

TEST(DeribitSignedRest, CancelPrefersTheVenueIdAndFallsBackToTheLabel) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    auto cmd = order_for(table, "BTC-PERPETUAL");
    cmd.kind = CmdKind::Cancel;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("BTC-4356791");
    const auto by_id = builder.cancel_order(cmd, table, now);
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->target, "/api/v2/private/cancel?order_id=BTC-4356791");

    // The ULID is unique per order, so "cancel everything with this label"
    // cancels exactly one — and the currency scope keeps it from reaching an
    // order on the other coin.
    cmd.target_exchange_order_id = {};
    const auto by_label = builder.cancel_order(cmd, table, now);
    ASSERT_TRUE(by_label.has_value());
    EXPECT_EQ(by_label->target,
              "/api/v2/private/cancel_by_label?label=01K3QW9F2H6M5N8P0R2T4V6X8Z&currency=BTC");

    cmd.exchange_client_order_id = {};
    EXPECT_EQ(builder.cancel_order(cmd, table, now).error(), BuildError::NoOrderTarget);
}

TEST(DeribitSignedRest, AmendNamesTheInstrumentWhenOnlyTheLabelIsKnown) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    auto cmd = order_for(table, "BTC-11AUG26-64000-C");
    cmd.kind = CmdKind::Amend;
    cmd.qty = QtyLots::from_raw(15);
    cmd.price = PriceTicks::from_raw(410);
    cmd.target_exchange_order_id = *ExchangeOrderId::from("BTC-4356792");
    const auto by_id = builder.amend_order(cmd, table, now);
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->target, "/api/v2/private/edit?order_id=BTC-4356792&amount=1.5&price=0.041");

    cmd.target_exchange_order_id = {};
    const auto by_label = builder.amend_order(cmd, table, now);
    ASSERT_TRUE(by_label.has_value());
    EXPECT_EQ(by_label->target,
              "/api/v2/private/edit_by_label?label=01K3QW9F2H6M5N8P0R2T4V6X8Z"
              "&instrument_name=BTC-11AUG26-64000-C&amount=1.5&price=0.041");

    // Deribit's edit demands at least one of amount and price; an empty one
    // would answer an error for a command that was a no-op to begin with.
    cmd.qty = QtyLots::undef();
    cmd.price = PriceTicks::undef();
    EXPECT_EQ(builder.amend_order(cmd, table, now).error(), BuildError::NothingToAmend);
}

TEST(DeribitSignedRest, PullsAreScopedByCurrencyAndByDeribitsOwnKind) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    // "future" answers for the perpetual AND the dated series; separating them
    // is the instrument table's job, not the query's.
    EXPECT_EQ(builder.open_orders(MarketType::Perp, "BTC", now)->target,
              "/api/v2/private/get_open_orders_by_currency?currency=BTC&kind=future");
    EXPECT_EQ(builder.open_orders(MarketType::Future, "ETH", now)->target,
              "/api/v2/private/get_open_orders_by_currency?currency=ETH&kind=future");
    EXPECT_EQ(builder.open_orders(MarketType::Option, "BTC", now)->target,
              "/api/v2/private/get_open_orders_by_currency?currency=BTC&kind=option");
    EXPECT_EQ(builder.positions(MarketType::Option, "BTC", now)->target,
              "/api/v2/private/get_positions?currency=BTC&kind=option");
    EXPECT_EQ(builder
                  .order_state_by_label(
                      *ExchangeClientOrderId::from("01K3QW9F2H6M5N8P0R2T4V6X8Z"), "BTC", now)
                  ->target,
              "/api/v2/private/get_order_state_by_label?currency=BTC"
              "&label=01K3QW9F2H6M5N8P0R2T4V6X8Z");

    EXPECT_EQ(builder.user_trades(MarketType::Perp, "BTC", UnixNanos{0}, now)->target,
              "/api/v2/private/get_user_trades_by_currency?currency=BTC&kind=future&count=1000");
    // Ascending from the watermark, so a full page leaves the OLDEST unseen
    // fills behind rather than the newest.
    EXPECT_EQ(builder.user_trades(MarketType::Perp, "BTC",
                                  UnixNanos{1'787'751'000'000'000'000ULL}, now)
                  ->target,
              "/api/v2/private/get_user_trades_by_currency?currency=BTC&kind=future&count=1000"
              "&start_timestamp=1787751000000&sorting=asc");

    EXPECT_EQ(builder.open_orders(MarketType::Spot, "BTC", now).error(),
              BuildError::SpotNotModelled);
}

TEST(DeribitSignedRest, RefusesToSignWithoutCredentials) {
    net::Signer signer; // no key, no secret
    SignedRequestBuilder builder{signer};
    // NotSigned rather than a request with an empty signature: an unsigned
    // order that reaches the venue is answered 13009, which reads like an
    // outage rather than a missing environment variable.
    EXPECT_EQ(builder.positions(MarketType::Perp, "BTC", UnixNanos{1}).error(),
              BuildError::NotSigned);
}

TEST(DeribitVenue, LabelCarriesAWholeUlid) {
    // 26 characters of Crockford base32 into a field capped at 64. If Deribit
    // ever shortened it, two orders sharing a prefix would answer one cancel —
    // which is why this is a static_assert in rest.hpp and not a comment.
    EXPECT_GE(k_max_label, sizeof(ExchangeClientOrderId::data));
    EXPECT_GE(exec::caps_of(VenueId::Deribit).max_client_order_id_len,
              exec::k_min_client_order_id_len);
    EXPECT_EQ(currency_of("BTC-11AUG26-64000-C"), "BTC");
    EXPECT_EQ(currency_of("ETH-PERPETUAL"), "ETH");
    EXPECT_TRUE(currency_of("NODASH").empty());
}

// ------------------------------------------------------ private wire ----

TEST(DeribitPrivateWire, AnAckCarriesTheOrderAndTheTradesItCaused) {
    const auto body = fixture("rest_private_buy_option_ack.json");
    auto storage = std::make_unique<UserTradeBuffer>();
    const auto ack = parse_order_ack(body, *storage);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->order.order_id, "BTC-4356790");
    EXPECT_EQ(ack->order.label, "01K3QW9F2H6M5N8P0R2T4V6X90");
    EXPECT_EQ(ack->order.state, "filled");
    EXPECT_EQ(ack->order.tif, "immediate_or_cancel");
    ASSERT_EQ(ack->trades.size(), 1U);
    EXPECT_EQ(ack->trades[0].trade_id, "BTC-901234");
    EXPECT_TRUE(ack->trades[0].liquidity_stated);
    EXPECT_FALSE(ack->trades[0].maker);

    const auto empty = fixture("rest_private_buy_ack.json");
    const auto resting = parse_order_ack(empty, *storage);
    ASSERT_TRUE(resting.has_value());
    EXPECT_TRUE(resting->trades.empty());
    EXPECT_EQ(resting->order.state, "open");
}

TEST(DeribitPrivateWire, AListAndASingleObjectCarryTheSameRow) {
    // private/cancel answers with one bare order object where the list reads
    // answer with an array; refusing either shape would drop half the replies.
    const auto listed = fixture("rest_open_orders.json");
    auto storage = std::make_unique<OrderBuffer>();
    const auto many = parse_orders(listed, *storage);
    ASSERT_TRUE(many.has_value());
    EXPECT_EQ(many->size(), 3U);

    const std::string single =
        R"({"jsonrpc":"2.0","id":1,"result":{"order_id":"BTC-4356791","order_state":"cancelled",)"
        R"("time_in_force":"good_til_cancelled","instrument_name":"BTC-PERPETUAL",)"
        R"("label":"01K3QW9F2H6M5N8P0R2T4V6X91","direction":"buy","price":63500.0,)"
        R"("amount":200.0,"filled_amount":60.0,"average_price":63500.0,)"
        R"("last_update_timestamp":1787751022000}})";
    const auto one = parse_orders(single, *storage);
    ASSERT_TRUE(one.has_value());
    ASSERT_EQ(one->size(), 1U);
    EXPECT_EQ((*one)[0].state, "cancelled");
}

TEST(DeribitPrivateWire, AChangesFrameDispatchesWithoutAnInstrumentInItsChannel) {
    const auto frame = fixture("ws_user_changes_partial_fill.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::UserChange);
    // The second channel segment is "changes", not a symbol; copying it into
    // EventInfo would hand the mapper a word to look up in the table.
    EXPECT_TRUE(event->instrument.empty());

    auto orders = std::make_unique<OrderBuffer>();
    auto trades = std::make_unique<UserTradeBuffer>();
    const auto changes = parse_user_changes(frame, *orders, *trades);
    ASSERT_TRUE(changes.has_value());
    ASSERT_EQ(changes->orders.size(), 1U);
    ASSERT_EQ(changes->trades.size(), 1U);
    EXPECT_EQ(changes->orders[0].order_id, changes->trades[0].order_id);
}

TEST(DeribitPrivateWire, MarketPriceIsNotANumberAndIsNotReadAsOne) {
    const auto frame = fixture("ws_user_changes_market_price.json");
    auto orders = std::make_unique<OrderBuffer>();
    auto trades = std::make_unique<UserTradeBuffer>();
    const auto changes = parse_user_changes(frame, *orders, *trades);
    // A trigger order prices itself with the WORD "market_price". The frame
    // still parses; only the price is absent, because a word is not a price.
    ASSERT_TRUE(changes.has_value());
    ASSERT_EQ(changes->orders.size(), 1U);
    EXPECT_TRUE(changes->orders[0].price.empty());
    EXPECT_EQ(changes->orders[0].state, "untriggered");
}

// ------------------------------------------------- order event mapping ----

TEST(DeribitMap, APartialFillReportsWhatTheOrderRowBesideItSaysIsLeft) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    const auto frame = fixture("ws_user_changes_partial_fill.json");
    auto orders = std::make_unique<OrderBuffer>();
    auto trades = std::make_unique<UserTradeBuffer>();
    const auto changes = parse_user_changes(frame, *orders, *trades);
    ASSERT_TRUE(changes.has_value());
    ASSERT_EQ(changes->orders.size(), 1U);

    // 200 USD ordered, 60 filled: 14 lots of 10 USD left. Deribit publishes no
    // leaves field, so it is the subtraction or nothing.
    const auto leaves = order_leaves(changes->orders[0], table);
    ASSERT_TRUE(leaves.has_value());
    EXPECT_EQ(leaves->raw, 14);

    const auto fill = map_fill(changes->trades[0], table, *leaves, UnixNanos{99});
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->side, Side::Buy);
    EXPECT_EQ(fill->qty.raw, 6);           // 60 USD / 10
    EXPECT_EQ(fill->price.raw, 127'000);   // 63500 / 0.5
    EXPECT_EQ(fill->leaves_qty.raw, 14);   // NOT zero: the order is still working
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
    EXPECT_EQ(fill->fee.raw, -140);        // -1.4e-7 BTC rebate, in the base coin
    EXPECT_EQ(fill->exec_id.view(), "BTC-901240");
    EXPECT_EQ(fill->exchange_client_order_id.view(), "01K3QW9F2H6M5N8P0R2T4V6X91");
    EXPECT_EQ(fill->ts_event.ns, 1'787'751'030'000ULL * 1'000'000ULL);

    // The same frame's order row says nothing new: the order is partially
    // filled, and Accepted is not a transition out of PartiallyFilled.
    EXPECT_FALSE(map_order_event(changes->orders[0], table, UnixNanos{99}).has_value());

    // An undefined leaves is refused rather than becoming a zero. The OMS
    // subtracts this field from the order size, so a zero here would close an
    // order that is still working.
    EXPECT_FALSE(
        map_fill(changes->trades[0], table, QtyLots::undef(), UnixNanos{99}).has_value());
}

TEST(DeribitMap, AnUntouchedOpenOrderIsTheOneRowThatMeansAccepted) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    const auto frame = fixture("ws_user_changes_accept.json");
    auto orders = std::make_unique<OrderBuffer>();
    auto trades = std::make_unique<UserTradeBuffer>();
    const auto changes = parse_user_changes(frame, *orders, *trades);
    ASSERT_TRUE(changes.has_value());
    ASSERT_EQ(changes->orders.size(), 1U);
    EXPECT_TRUE(changes->trades.empty());

    const auto event = map_order_event(changes->orders[0], table, UnixNanos{5});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Accepted);
    EXPECT_EQ(event->side, Side::Sell);
    EXPECT_EQ(event->instrument.market(), MarketType::Option);
    EXPECT_EQ(event->qty.raw, 15);        // 1.5 coin / 0.1
    EXPECT_EQ(event->leaves_qty.raw, 15);
    EXPECT_EQ(event->price.raw, 410);     // 0.041 / 0.0001
    EXPECT_EQ(event->exchange_order_id.view(), "BTC-4356792");
}

TEST(DeribitMap, AnIocRemainderExpiresWhereAWithdrawnOrderIsCancelled) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    const auto ioc = fixture("rest_order_state_ioc_cancelled.json");
    auto storage = std::make_unique<OrderBuffer>();
    const auto rows = parse_orders(ioc, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);

    // Deribit has no "expired" state, so the time-in-force is the only witness
    // to an IOC working as designed rather than the venue pulling an order out
    // from under a strategy.
    const auto event = map_order_event((*rows)[0], table, UnixNanos{7});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Expired);
    EXPECT_EQ(event->leaves_qty.raw, 18); // 300 - 120 USD, in 10 USD lots

    const auto report = map_open_order((*rows)[0], table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Expired);
    EXPECT_EQ(report->qty.raw, 30);
    EXPECT_EQ(report->leaves.raw, 18);
}

TEST(DeribitMap, OpenOrdersSeparateThePerpetualFromTheSeriesSharingItsKind) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    const auto listed = fixture("rest_open_orders.json");
    auto storage = std::make_unique<OrderBuffer>();
    const auto rows = parse_orders(listed, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);

    const auto perp = map_open_order((*rows)[0], table);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(perp->instrument.market(), MarketType::Perp);
    EXPECT_EQ(perp->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(perp->leaves.raw, 14);

    const auto option = map_open_order((*rows)[1], table);
    ASSERT_TRUE(option.has_value());
    EXPECT_EQ(option->instrument.market(), MarketType::Option);
    EXPECT_EQ(option->status, OrderStatus::Accepted);

    // An instrument this process never loaded is DROPPED, never invented: the
    // sweep is currency-wide and can name series we do not model.
    EXPECT_FALSE(map_open_order((*rows)[2], table).has_value());
}

TEST(DeribitMap, FillReportsAndPositionsCarryTheirOwnUnits) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);

    const auto trades_body = fixture("rest_user_trades.json");
    auto trade_storage = std::make_unique<UserTradeBuffer>();
    const auto trades = parse_user_trades(trades_body, *trade_storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 2U);

    const auto maker = map_fill_report((*trades)[0], table);
    ASSERT_TRUE(maker.has_value());
    EXPECT_EQ(maker->liquidity, Liquidity::Maker);
    EXPECT_EQ(maker->qty.raw, 6);
    EXPECT_EQ(maker->fee.raw, -140);
    const auto taker = map_fill_report((*trades)[1], table);
    ASSERT_TRUE(taker.has_value());
    EXPECT_EQ(taker->liquidity, Liquidity::Taker);
    EXPECT_EQ(taker->instrument.market(), MarketType::Option);
    EXPECT_EQ(taker->qty.raw, 3);       // 0.3 coin / 0.1
    EXPECT_EQ(taker->fee.raw, 90'000);  // 9.0e-5 BTC

    const auto positions_body = fixture("rest_positions.json");
    auto position_storage = std::make_unique<PositionBuffer>();
    const auto positions = parse_positions(positions_body, *position_storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 2U);

    // The SIGN is the direction; there is no separate side to disagree with it.
    const auto short_perp = map_position((*positions)[0], table, UnixNanos{11});
    ASSERT_TRUE(short_perp.has_value());
    EXPECT_EQ(short_perp->net_qty.raw, -20); // -200 USD in 10 USD lots
    EXPECT_EQ(short_perp->avg_entry.raw, 128'000);
    EXPECT_EQ(short_perp->ts_venue.ns, 11U);

    const auto long_option = map_position((*positions)[1], table, UnixNanos{11});
    ASSERT_TRUE(long_option.has_value());
    EXPECT_EQ(long_option->net_qty.raw, 25); // 2.5 coin in 0.1 lots
    EXPECT_EQ(long_option->avg_entry.raw, 315);
}

// --------------------------------------------------------------- pnl ----

// The quote value of ONE lot, which is the product portfolio::settlement_value
// multiplies by (or divides into) a price. Asserting the product rather than
// its factors is deliberate: lot_size and contract_value can be split many
// ways and only their product is arithmetic.
std::int64_t quote_per_lot(const InstrumentBase& base) {
    return mul_div(base.lot_size.raw, base.contract_value.raw, k_e9);
}

TEST(DeribitPnl, OneLotIsWorthWhatDeribitSizesAnOrderIn) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);

    const auto* perp = table.find("BTC-PERPETUAL");
    ASSERT_NE(perp, nullptr);
    const auto& future_base = base_of(*perp);
    // 10 USD of face value per lot. portfolio's inverse formula is
    // qty x lot_size x contract_value / price, so 20 lots at 63 500 USD/BTC is
    // 200 / 63500 = 0.0031496 BTC of exposure. Leaving contract_value at the
    // venue's own contract_size of 10 — beside a lot_size that is ALREADY the
    // 10 USD block — would answer ten times that, and every PnL taken from it
    // would be wrong by the same factor. tests/portfolio/pnl_test.cpp models
    // this same instrument as lot 1 / contract_value 10: a different split of
    // the identical product, which is why the product is what is pinned here.
    EXPECT_EQ(future_base.kind, ContractKind::Inverse);
    EXPECT_EQ(quote_per_lot(future_base), 10 * k_e9);

    const auto* call = table.find("BTC-11AUG26-64000-C");
    ASSERT_NE(call, nullptr);
    const auto& option_base = base_of(*call);
    // LINEAR, though the catalogue calls this row "reversed" exactly like the
    // future above it. A BTC option quotes its premium in BTC and sizes itself
    // in BTC: 1.5 coin at 0.041 is 0.0615 BTC paid, a multiply. Inheriting the
    // future's arithmetic would price that premium by DIVIDING.
    EXPECT_EQ(option_base.kind, ContractKind::Linear);
    EXPECT_EQ(quote_per_lot(option_base), k_e9 / 10); // 0.1 coin per lot
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

TEST(DeribitErrors, ARefusalNamingAFinishedOrderIsNotACancelReject) {
    // The order finished between the decision and the request. Escalating
    // would raise a CancelRejected against an order the FSM has already
    // closed, so these are swallowed and reconciliation settles the rest.
    EXPECT_TRUE(is_settled_order_refusal(k_order_not_found));
    EXPECT_TRUE(is_settled_order_refusal(k_already_closed));
    EXPECT_TRUE(is_settled_order_refusal(k_already_filled));
    EXPECT_TRUE(is_settled_order_refusal(k_not_open_order));
    EXPECT_TRUE(is_settled_order_refusal(k_edit_replaced));
    // Not settled: the account is gone, and pretending otherwise hides it.
    EXPECT_FALSE(is_settled_order_refusal(k_account_locked));
    EXPECT_FALSE(is_settled_order_refusal(k_not_enough_funds));
}

TEST(DeribitErrors, LiveRefusalsParseAsTheEnvelopeTheyArriveIn) {
    // Both bodies are verbatim captures.
    const auto unauthorized = fixture("rest_error_unauthorized.json");
    const auto refused = parse_response(unauthorized);
    ASSERT_TRUE(refused.has_value());
    ASSERT_TRUE(refused->failed());
    EXPECT_EQ(refused->error_code, k_unauthorized);
    EXPECT_EQ(refused->error_message, "unauthorized");
    EXPECT_EQ(classify(refused->error_code), exec::ErrorClass::AuthFailure);

    // NEGATIVE codes share the field with Deribit's own positive ones, which
    // is why the "no error" sentinel cannot be zero.
    const auto missing = fixture("rest_error_method_not_found.json");
    const auto not_found = parse_response(missing);
    ASSERT_TRUE(not_found.has_value());
    ASSERT_TRUE(not_found->failed());
    EXPECT_EQ(not_found->error_code, k_method_not_found);
    EXPECT_NE(k_no_error, 0);

    // A successful envelope must not read as an error whatever its result is.
    const auto ok = fixture("rest_private_buy_ack.json");
    const auto accepted = parse_response(ok);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_FALSE(accepted->failed());
}

} // namespace
} // namespace raikiri::venues::deribit
