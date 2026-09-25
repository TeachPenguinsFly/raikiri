#include <raikiri/common/int128.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/sign.hpp>
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

std::size_t load_catalogue(exec::InstrumentTable& table, MarketType market, const std::string& body) {
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

// deribit's worked auth example, verbatim
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

TEST(DeribitVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue({.venue = VenueId::Deribit, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Deribit);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(DeribitVenue, EveryMarketItServesCanActuallyBeBuilt) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    for (const auto market : {MarketType::Perp, MarketType::Future, MarketType::Option}) {
        const auto adapter = exec::make_venue({.venue = VenueId::Deribit, .market = market}, sink, clock);
        ASSERT_TRUE(adapter.has_value()) << to_string(market);
        EXPECT_NE(adapter->instruments, nullptr) << to_string(market);
    }
    const auto spot = exec::make_venue({.venue = VenueId::Deribit, .market = MarketType::Spot}, sink, clock);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error(), exec::MakeError::UnsupportedMarket);
}

TEST(DeribitVenue, ServesEveryMarketItListsExceptSpot) {
    // the refusal is decided before the instrument table is consulted
    EXPECT_FALSE(market_refusal(MarketType::Perp).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Future).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Option).has_value());
    EXPECT_EQ(market_refusal(MarketType::Spot), BuildError::SpotNotModelled);
}

TEST(DeribitVenue, OrderEntryWithoutCredentialsFailsAtTheConfigNotTheVenue) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Deribit, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // AuthFailure sends an operator to the config
    const auto started = adapter->execution->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().cls, exec::ErrorClass::AuthFailure);
    EXPECT_FALSE(adapter->execution->connected());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{7});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    // an order that cannot be built must still be answered
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
    auto adapter = exec::make_venue({.venue = VenueId::Deribit, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // an empty vector would read as holding no spot
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Spot).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Spot, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Spot).has_value());
}

TEST(DeribitNumber, ReadsEveryFormDeribitEmits) {
    // live spellings; parse_decimal_e9 alone cannot read them
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
    // eighteen fractional digits is refused, not rounded
    EXPECT_FALSE(parse_number_e9("0.010543991548811414").has_value());
    EXPECT_FALSE(parse_number_e9("1e-10").has_value()); // below FixedE9's scale
    EXPECT_FALSE(parse_number_e9("").has_value());
    EXPECT_FALSE(parse_number_e9("\"64112.0\"").has_value()); // a quoted number
    EXPECT_FALSE(parse_number_e9("1.2.3").has_value());
    EXPECT_FALSE(parse_number_e9("1e").has_value());
    EXPECT_FALSE(parse_number_e9("NaN").has_value());
    EXPECT_FALSE(parse_number_e9("1e30").has_value()); // past int64 at 1e9 scale
}

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
    EXPECT_EQ(base.min_notional.raw, 10 * k_e9); // a usd notional here
    EXPECT_EQ(base.kind, ContractKind::Inverse); // settles in BTC, priced in USD
    // one: lot_size is already the 10 usd block, so 10 here would price a lot at 100 usd
    EXPECT_EQ(base.contract_value.raw, k_e9);
    // continuous accrual, so no interval
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
    // BTC-10AUG26 is delivered, ADA_USDC-PERPETUAL is linear, the rest are other markets
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

    // contract_size 1.0 would drop a real 23.1 quote, so min_trade_amount is the step
    const auto& base = base_of(*call);
    EXPECT_EQ(base.tick_size.raw, 100'000);       // 0.0001 BTC
    EXPECT_EQ(base.lot_size.raw, 100'000'000);    // 0.1 coin contracts
    EXPECT_EQ(base.contract_value.raw, 1 * k_e9); // 1.0 coin per contract

    // strike is stored in the premium's tick though priced in usd; read it through ticks_to_price
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

TEST(DeribitWire, DispatchesOnTheChannelPrefixAndItsShape) {
    // the two book streams differ only in their parameter count
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

    // an unknown instrument yields an empty list and no error
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

    // rest shares the envelope, with negative json-rpc codes
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
    // the action is the removal, not the size
    ASSERT_GE(change->bids.size(), 2U);
    EXPECT_EQ(change->bids[1].action, LevelAction::Delete);
    ASSERT_GE(change->asks.size(), 3U);
    EXPECT_EQ(change->asks[0].action, LevelAction::Change);
    EXPECT_EQ(change->asks[2].price, "1.641e5"); // an exponent-form price
}

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
    // 23.1 contracts is not a multiple of contract_size 1.0
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
    // 40 usd / 10 usd = 4 lots
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
    // funding accrues continuously; eight hours is the rate's unit
    EXPECT_EQ(funding->next_funding_time.ns, 0U);
    EXPECT_EQ(funding->interval_s, 8U * 60U * 60U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Continuous);
    // deribit publishes no forecast
    EXPECT_TRUE(funding->predicted_rate.is_undef());
}

TEST(DeribitMap, MarkPriceIsDroppedWhenItIsNotOnTheTick) {
    // the perpetual mark is off the 0.5 tick, so it stays undefined; option marks sit on the tick
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
    // a dated future has no funding
    EXPECT_FALSE(map_funding(*future_wire, futures, UnixNanos{1}).has_value());

    exec::InstrumentTable options;
    ASSERT_EQ(load_catalogue(options, MarketType::Option, catalogue), 2U);
    const auto option_frame = fixture("ws_ticker_option.json");
    const auto option_wire = parse_ticker(option_frame);
    ASSERT_TRUE(option_wire.has_value());
    EXPECT_EQ(option_wire->mark_price, "0.0044");
    // an option pays no funding
    EXPECT_FALSE(map_funding(*option_wire, options, UnixNanos{1}).has_value());
    EXPECT_TRUE(map_greeks(*option_wire, options, UnixNanos{1}).has_value());
}

TEST(DeribitMap, TheStatsBlockCarriesBothColumnsOnEveryKindOnThisChannel) {
    const auto catalogue = fixture("rest_get_instruments.json");

    exec::InstrumentTable perps;
    ASSERT_EQ(load_catalogue(perps, MarketType::Perp, catalogue), 1U);
    // named: the parsed views point into the frame
    const auto perp_frame = fixture("ws_ticker_perpetual.json");
    const auto perp = parse_ticker(perp_frame);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(perp->stats_volume, "2934.85682569");
    EXPECT_EQ(perp->stats_volume_usd, "189879910.0");
    const auto stats = map_ticker_stats(*perp, perps, UnixNanos{11});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 2'934'856'825'690);
    EXPECT_EQ(stats->volume_24h_quote.raw, 189'879'910'000'000);
    // volume is base even coin-margined: the ratio is a plausible btc price
    EXPECT_EQ(stats->volume_24h_quote.raw * (k_e9 / k_e6) / stats->volume_24h_base.raw, 64'698);
    EXPECT_EQ(stats->ts_event.ns, 1'786'395'070'130'000'000ULL);
    EXPECT_EQ(stats->ts_init.ns, 11U);

    exec::InstrumentTable futures;
    ASSERT_EQ(load_catalogue(futures, MarketType::Future, catalogue), 1U);
    const auto future_frame = fixture("ws_ticker_future.json");
    const auto future = parse_ticker(future_frame);
    ASSERT_TRUE(future.has_value());
    const auto future_stats = map_ticker_stats(*future, futures, UnixNanos{11});
    ASSERT_TRUE(future_stats.has_value());
    EXPECT_EQ(future_stats->volume_24h_base.raw, 155'337'460'510);
    EXPECT_EQ(future_stats->volume_24h_quote.raw, 10'129'490'000'000);
    EXPECT_EQ(future_stats->volume_24h_quote.raw * (k_e9 / k_e6) / future_stats->volume_24h_base.raw, 65'209);

    exec::InstrumentTable options;
    ASSERT_EQ(load_catalogue(options, MarketType::Option, catalogue), 2U);
    const auto option_frame = fixture("ws_ticker_option.json");
    const auto option = parse_ticker(option_frame);
    ASSERT_TRUE(option.has_value());
    const auto option_stats = map_ticker_stats(*option, options, UnixNanos{11});
    ASSERT_TRUE(option_stats.has_value());
    // an option's quote side is the premium traded
    EXPECT_EQ(option_stats->volume_24h_base.raw, 15'600'000'000);
    EXPECT_EQ(option_stats->volume_24h_quote.raw, 4'521'240'000);
    EXPECT_EQ(option_stats->volume_24h_quote.raw * (k_e9 / k_e6) / option_stats->volume_24h_base.raw, 289);
}

TEST(DeribitMap, ATickerWithoutStatsStillYieldsItsFunding) {
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable perps;
    ASSERT_EQ(load_catalogue(perps, MarketType::Perp, catalogue), 1U);
    const auto frame = fixture("ws_ticker_perpetual.json");
    auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    wire->stats_volume = {};
    wire->stats_volume_usd = {};
    // absent stats is not zero
    EXPECT_FALSE(map_ticker_stats(*wire, perps, UnixNanos{11}).has_value());
    EXPECT_TRUE(map_funding(*wire, perps, UnixNanos{11}).has_value());
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

    // percent on the wire, ratio in the domain
    EXPECT_EQ(greeks->mark_iv.raw, 252'400'000);
    EXPECT_EQ(greeks->bid_iv.raw, 218'400'000);
    EXPECT_EQ(greeks->ask_iv.raw, 275'900'000);
    EXPECT_LT(greeks->bid_iv.raw, greeks->ask_iv.raw);

    // the underlying is a dollar price, not ticks
    EXPECT_EQ(greeks->underlying_price.raw, 64'107'290'000'000);
}

TEST(DeribitMap, APerpetualTickerCarriesNoGreeksAtAll) {
    // an option with zero greeks must differ from an instrument with none
    const auto catalogue = fixture("rest_get_instruments.json");
    exec::InstrumentTable perps;
    ASSERT_GT(load_catalogue(perps, MarketType::Perp, catalogue), 0U);

    const auto frame = fixture("ws_ticker_perpetual.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(has_greeks(*wire));
    EXPECT_FALSE(map_greeks(*wire, perps, UnixNanos{1}).has_value());
    EXPECT_TRUE(map_funding(*wire, perps, UnixNanos{1}).has_value());
}

TEST(DeribitMap, UnknownInstrumentIsDroppedRatherThanInvented) {
    const exec::InstrumentTable empty;
    const auto frame = fixture("ws_book_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_bbo(*wire, empty, UnixNanos{1}).has_value());
}

// the parsed views point into the frame and level buffer, so both must outlive the wire struct
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

// bids descend, asks ascend, the sides never cross and every quantity is positive
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
    // ids are not contiguous, so only the back-pointer decides
    EXPECT_GT(change.change_id - snapshot.change_id, 1);
    EXPECT_EQ(book.apply(change), BookResult::Applied);

    const auto out = book.snapshot(UnixNanos{7}, UnixNanos{9});
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    // deribit pushes the whole book; shm takes the top
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
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot_full.json")), BookResult::Resynced);

    const auto before = book.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->asks[16].price.raw, 128'092) << "64046.0, the level the delta deletes";

    EXPECT_EQ(book.apply(book_of(change_frame, "ws_book_change_full.json")), BookResult::Applied);
    const auto after = book.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(after.has_value());

    const auto gone = std::find_if(after->asks.begin(),
                                   after->asks.begin() + after->ask_count,
                                   [](const BookLevel& level) { return level.price.raw == 128'092; });
    EXPECT_EQ(gone, after->asks.begin() + after->ask_count) << "the delete removed it";
    const auto added = std::find_if(after->asks.begin(),
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
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot_full.json")), BookResult::Resynced);

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
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot_full.json")), BookResult::Resynced);
    const auto change = book_of(change_frame, "ws_book_change_full.json");
    EXPECT_EQ(book.apply(change), BookResult::Applied);

    EXPECT_EQ(book.apply(change), BookResult::Ignored);
    EXPECT_EQ(book.gaps(), 0U);
    EXPECT_TRUE(book.synced());
    EXPECT_EQ(book.last_change_id(), change.change_id);
}

TEST(DeribitBook, ALevelOffTheStepInvalidatesTheWholeBook) {
    const auto table = perp_catalogue();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    const auto snapshot = book_of(snapshot_frame, "ws_book_snapshot_full.json");
    EXPECT_EQ(book.apply(snapshot), BookResult::Resynced);

    const std::array<WireLevel, 1> bad{WireLevel{.action = LevelAction::Change, .price = "64035.25", .qty = "1.0e3"}};
    WireBook off_tick{};
    off_tick.instrument = "BTC-PERPETUAL";
    off_tick.change_id = snapshot.change_id + 1;
    off_tick.prev_change_id = snapshot.change_id;
    off_tick.bids = bad;
    EXPECT_EQ(book.apply(off_tick), BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());

    // a size finer than the 10 usd step is refused too, deletes included
    auto again = book_for(table);
    EXPECT_EQ(again.apply(snapshot), BookResult::Resynced);
    const std::array<WireLevel, 1> sub_lot{WireLevel{.action = LevelAction::Delete, .price = "64035.25", .qty = "0.0"}};
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
    EXPECT_EQ(channel_for(exec::SubKind::Bbo, "BTC-PERPETUAL", buffer), "book.BTC-PERPETUAL.none.1.100ms");
    EXPECT_EQ(channel_for(exec::SubKind::Trades, "BTC-PERPETUAL", buffer), "trades.BTC-PERPETUAL.100ms");
    EXPECT_EQ(channel_for(exec::SubKind::Depth, "BTC-PERPETUAL", buffer), "book.BTC-PERPETUAL.100ms");
    // funding and mark share one ticker channel
    EXPECT_EQ(channel_for(exec::SubKind::Funding, "BTC-PERPETUAL", buffer), "ticker.BTC-PERPETUAL.100ms");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice, "BTC-PERPETUAL", buffer), "ticker.BTC-PERPETUAL.100ms");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline, "BTC-PERPETUAL", buffer).empty());
    EXPECT_TRUE(channel_for(exec::SubKind::Bbo, "", buffer).empty());

    // too small a buffer yields empty, not a prefix
    std::array<char, 8> tiny{};
    EXPECT_TRUE(channel_for(exec::SubKind::Bbo, "BTC-PERPETUAL", tiny).empty());
}

TEST(DeribitWs, SubscribeFramesAreJsonRpcAndCapped) {
    Protocol protocol;
    const std::array<Subscription, 2> two{Subscription{.channel = "book.BTC-PERPETUAL.none.1.100ms"},
                                          Subscription{.channel = "trades.BTC-PERPETUAL.100ms"}};
    const auto frame = protocol.subscribe_frame(two);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(frame->text,
              R"({"jsonrpc":"2.0","id":1,"method":"public/subscribe","params":{"channels":)"
              R"(["book.BTC-PERPETUAL.none.1.100ms","trades.BTC-PERPETUAL.100ms"]}})");

    const auto next = protocol.subscribe_frame(two);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->id, 2);

    // one refused channel fails the whole batch
    const std::array<Subscription, 2> poisoned{Subscription{.channel = "trades.BTC-PERPETUAL.100ms"},
                                               Subscription{.channel = ""}};
    EXPECT_FALSE(protocol.subscribe_frame(poisoned).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());

    std::array<Subscription, 260> many{};
    for (auto& entry : many) {
        entry.channel = "ticker.BTC-PERPETUAL.100ms";
    }
    const auto capped = protocol.subscribe_frame(many);
    ASSERT_TRUE(capped.has_value());
    EXPECT_EQ(capped->consumed, many.size());
}

TEST(DeribitWs, HeartbeatFramesMatchWhatDeribitAnswered) {
    Protocol protocol;
    EXPECT_EQ(protocol.set_heartbeat_frame(15).text,
              R"({"jsonrpc":"2.0","id":1,"method":"public/set_heartbeat","params":{"interval":15}})");
    EXPECT_EQ(protocol.test_frame().text, R"({"jsonrpc":"2.0","id":2,"method":"public/test","params":{}})");
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
    EXPECT_EQ(auth->consumed, 0U);

    std::array<char, k_max_channel_len> buffer{};
    EXPECT_EQ(user_changes_channel("future", "BTC", buffer), "user.changes.future.BTC.raw");
    EXPECT_EQ(user_changes_channel("option", "ETH", buffer), "user.changes.option.ETH.raw");
    EXPECT_TRUE(user_changes_channel("future", {}, buffer).empty());

    const std::array<Subscription, 2> channels{Subscription{.channel = "user.changes.future.BTC.raw"},
                                               Subscription{.channel = "user.changes.option.BTC.raw"}};
    const auto subscribe = protocol.private_subscribe_frame(channels);
    ASSERT_TRUE(subscribe.has_value());
    EXPECT_EQ(subscribe->text,
              R"({"jsonrpc":"2.0","id":2,"method":"private/subscribe","params":{"channels":)"
              R"(["user.changes.future.BTC.raw","user.changes.option.BTC.raw"]}})");
    EXPECT_EQ(subscribe->consumed, 2U);

    Protocol bare_protocol;
    const net::Signer bare;
    EXPECT_FALSE(bare_protocol.auth_frame(bare, UnixNanos{1}).has_value());
}

TEST(DeribitWs, TestnetIsASeparateHost) {
    EXPECT_EQ(stream_host(false), "www.deribit.com");
    EXPECT_EQ(stream_host(true), "test.deribit.com");
    EXPECT_EQ(stream_path(), "/ws/api/v2");
}

TEST(DeribitRest, TargetsNameTheMethodInThePath) {
    RequestBuilder builder;
    EXPECT_EQ(builder.server_time().target, "/api/v2/public/get_time");
    // there is no perpetual kind; perpetuals ride the future call
    EXPECT_EQ(builder.instruments("BTC", "future").target, "/api/v2/public/get_instruments?currency=BTC&kind=future");
    EXPECT_EQ(builder.instruments("ETH", "option").target, "/api/v2/public/get_instruments?currency=ETH&kind=option");
    EXPECT_EQ(rest_host(false), "www.deribit.com");
    EXPECT_EQ(rest_host(true), "test.deribit.com");
}

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
    // values from deribit's client_signature docs; both newlines matter
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
    cmd.qty = QtyLots::from_raw(20);           // 20 x 10 USD = 200 USD
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
    // digest computed independently with openssl dgst -sha256 -hmac
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

    // deribit defaults post_only to true, so it is always sent
    auto market = order_for(table, "BTC-PERPETUAL");
    market.side = Side::Sell;
    market.order_type = OrderType::Market;
    market.qty = QtyLots::from_raw(3);
    const auto sell = builder.create_order(market, table, now);
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->target,
              "/api/v2/private/sell?instrument_name=BTC-PERPETUAL&amount=30"
              "&label=01K3QW9F2H6M5N8P0R2T4V6X8Z&type=market&post_only=false");

    // a crossing post-only order is rejected, not repriced
    auto maker = order_for(table, "BTC-PERPETUAL");
    maker.flags = OrderFlags::PostOnly | OrderFlags::ReduceOnly;
    maker.qty = QtyLots::from_raw(1);
    maker.price = PriceTicks::from_raw(120'000);
    const auto reduce = builder.create_order(maker, table, now);
    ASSERT_TRUE(reduce.has_value());
    EXPECT_NE(std::string{reduce->target}.find("&post_only=true&reject_post_only=true"), std::string::npos);
    EXPECT_NE(std::string{reduce->target}.find("&reduce_only=true"), std::string::npos);
}

TEST(DeribitSignedRest, AnOptionOrderIsTheSameCallWithAnotherInstrumentName) {
    // one private/buy shape: the instrument decides the unit of amount
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
    call.qty = QtyLots::from_raw(15);       // 15 x 0.1 = 1.5 coin
    call.price = PriceTicks::from_raw(410); // 410 x 0.0001 = 0.041 BTC premium
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

    cmd.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{999});
    cmd.price = PriceTicks::from_raw(127'000);
    EXPECT_EQ(builder.create_order(cmd, table, now).error(), BuildError::UnknownInstrument);

    // spot is refused before the table is read
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

    cmd.target_exchange_order_id = {};
    const auto by_label = builder.cancel_order(cmd, table, now);
    ASSERT_TRUE(by_label.has_value());
    EXPECT_EQ(by_label->target, "/api/v2/private/cancel_by_label?label=01K3QW9F2H6M5N8P0R2T4V6X8Z&currency=BTC");

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

    // edit needs at least one of amount and price
    cmd.qty = QtyLots::undef();
    cmd.price = PriceTicks::undef();
    EXPECT_EQ(builder.amend_order(cmd, table, now).error(), BuildError::NothingToAmend);
}

TEST(DeribitSignedRest, PullsAreScopedByCurrencyAndByDeribitsOwnKind) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    EXPECT_EQ(builder.open_orders(MarketType::Perp, "BTC", now)->target,
              "/api/v2/private/get_open_orders_by_currency?currency=BTC&kind=future");
    EXPECT_EQ(builder.open_orders(MarketType::Future, "ETH", now)->target,
              "/api/v2/private/get_open_orders_by_currency?currency=ETH&kind=future");
    EXPECT_EQ(builder.open_orders(MarketType::Option, "BTC", now)->target,
              "/api/v2/private/get_open_orders_by_currency?currency=BTC&kind=option");
    EXPECT_EQ(builder.positions(MarketType::Option, "BTC", now)->target,
              "/api/v2/private/get_positions?currency=BTC&kind=option");
    EXPECT_EQ(
        builder.order_state_by_label(*ExchangeClientOrderId::from("01K3QW9F2H6M5N8P0R2T4V6X8Z"), "BTC", now)->target,
        "/api/v2/private/get_order_state_by_label?currency=BTC"
        "&label=01K3QW9F2H6M5N8P0R2T4V6X8Z");

    EXPECT_EQ(builder.user_trades(MarketType::Perp, "BTC", UnixNanos{0}, now)->target,
              "/api/v2/private/get_user_trades_by_currency?currency=BTC&kind=future&count=1000");
    EXPECT_EQ(builder.user_trades(MarketType::Perp, "BTC", UnixNanos{1'787'751'000'000'000'000ULL}, now)->target,
              "/api/v2/private/get_user_trades_by_currency?currency=BTC&kind=future&count=1000"
              "&start_timestamp=1787751000000&sorting=asc");

    EXPECT_EQ(builder.open_orders(MarketType::Spot, "BTC", now).error(), BuildError::SpotNotModelled);
}

TEST(DeribitSignedRest, RefusesToSignWithoutCredentials) {
    net::Signer signer; // no key, no secret
    SignedRequestBuilder builder{signer};
    // unsigned requests are answered 13009, which reads like an outage
    EXPECT_EQ(builder.positions(MarketType::Perp, "BTC", UnixNanos{1}).error(), BuildError::NotSigned);
}

TEST(DeribitVenue, LabelCarriesAWholeUlid) {
    EXPECT_GE(k_max_label, sizeof(ExchangeClientOrderId::data));
    EXPECT_GE(exec::caps_of(VenueId::Deribit).max_client_order_id_len, exec::k_min_client_order_id_len);
    EXPECT_EQ(currency_of("BTC-11AUG26-64000-C"), "BTC");
    EXPECT_EQ(currency_of("ETH-PERPETUAL"), "ETH");
    EXPECT_TRUE(currency_of("NODASH").empty());
}

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
    // private/cancel answers a bare object, list reads an array
    const auto listed = fixture("rest_open_orders.json");
    auto storage = std::make_unique<OrderBuffer>();
    const auto many = parse_orders(listed, *storage);
    ASSERT_TRUE(many.has_value());
    EXPECT_EQ(many->size(), 3U);

    const std::string
        single = R"({"jsonrpc":"2.0","id":1,"result":{"order_id":"BTC-4356791","order_state":"cancelled",)"
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
    // a trigger order's price is the word market_price; only the price is dropped
    ASSERT_TRUE(changes.has_value());
    ASSERT_EQ(changes->orders.size(), 1U);
    EXPECT_TRUE(changes->orders[0].price.empty());
    EXPECT_EQ(changes->orders[0].state, "untriggered");
}

TEST(DeribitMap, APartialFillReportsWhatTheOrderRowBesideItSaysIsLeft) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);
    const auto frame = fixture("ws_user_changes_partial_fill.json");
    auto orders = std::make_unique<OrderBuffer>();
    auto trades = std::make_unique<UserTradeBuffer>();
    const auto changes = parse_user_changes(frame, *orders, *trades);
    ASSERT_TRUE(changes.has_value());
    ASSERT_EQ(changes->orders.size(), 1U);

    // 200 usd ordered, 60 filled: 14 lots of 10 usd left
    const auto leaves = order_leaves(changes->orders[0], table);
    ASSERT_TRUE(leaves.has_value());
    EXPECT_EQ(leaves->raw, 14);

    const auto fill = map_fill(changes->trades[0], table, *leaves, UnixNanos{99});
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->side, Side::Buy);
    EXPECT_EQ(fill->qty.raw, 6);         // 60 USD / 10
    EXPECT_EQ(fill->price.raw, 127'000); // 63500 / 0.5
    EXPECT_EQ(fill->leaves_qty.raw, 14); // not zero: the order is still working
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
    EXPECT_EQ(fill->fee.raw, -140); // -1.4e-7 BTC rebate, in the base coin
    EXPECT_EQ(fill->exec_id.view(), "BTC-901240");
    EXPECT_EQ(fill->exchange_client_order_id.view(), "01K3QW9F2H6M5N8P0R2T4V6X91");
    EXPECT_EQ(fill->ts_event.ns, 1'787'751'030'000ULL * 1'000'000ULL);

    EXPECT_FALSE(map_order_event(changes->orders[0], table, UnixNanos{99}).has_value());

    // undefined leaves is refused, not read as zero
    EXPECT_FALSE(map_fill(changes->trades[0], table, QtyLots::undef(), UnixNanos{99}).has_value());
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
    EXPECT_EQ(event->qty.raw, 15); // 1.5 coin / 0.1
    EXPECT_EQ(event->leaves_qty.raw, 15);
    EXPECT_EQ(event->price.raw, 410); // 0.041 / 0.0001
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

    // a currency-wide sweep can name series not loaded here
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
    EXPECT_EQ(taker->qty.raw, 3);      // 0.3 coin / 0.1
    EXPECT_EQ(taker->fee.raw, 90'000); // 9.0e-5 BTC

    const auto positions_body = fixture("rest_positions.json");
    auto position_storage = std::make_unique<PositionBuffer>();
    const auto positions = parse_positions(positions_body, *position_storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 2U);

    // the sign is the direction
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

// quote value of one lot; only the product of lot_size and contract_value matters
std::int64_t quote_per_lot(const InstrumentBase& base) {
    return mul_div(base.lot_size.raw, base.contract_value.raw, k_e9);
}

TEST(DeribitPnl, OneLotIsWorthWhatDeribitSizesAnOrderIn) {
    const auto body = fixture("rest_get_instruments.json");
    const auto table = full_catalogue(body);

    const auto* perp = table.find("BTC-PERPETUAL");
    ASSERT_NE(perp, nullptr);
    const auto& future_base = base_of(*perp);
    // 20 lots at 63500 is 200 / 63500 btc of exposure; pnl_test splits the same product differently
    EXPECT_EQ(future_base.kind, ContractKind::Inverse);
    EXPECT_EQ(quote_per_lot(future_base), 10 * k_e9);

    const auto* call = table.find("BTC-11AUG26-64000-C");
    ASSERT_NE(call, nullptr);
    const auto& option_base = base_of(*call);
    // linear though the row says reversed: premium and size are both in btc
    EXPECT_EQ(option_base.kind, ContractKind::Linear);
    EXPECT_EQ(quote_per_lot(option_base), k_e9 / 10); // 0.1 coin per lot
}

TEST(DeribitErrors, ClassificationKeepsRecoverableThingsRecoverable) {
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_too_many_concurrent_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_retry), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_settlement_in_progress), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_temporarily_unavailable), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_system_maintenance), exec::ErrorClass::Retryable);
    // the session lapsed; logging in again fixes it
    EXPECT_EQ(classify(k_authorization_required), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_subscribe_error_unsubscribed), exec::ErrorClass::Retryable);

    EXPECT_EQ(classify(k_invalid_credentials), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_unauthorized), exec::ErrorClass::AuthFailure);

    // unknown codes in both spaces default to terminal
    EXPECT_EQ(classify(k_invalid_params), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_method_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_raw_subscriptions_unauthorized), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
}

TEST(DeribitErrors, ARefusalNamingAFinishedOrderIsNotACancelReject) {
    EXPECT_TRUE(is_settled_order_refusal(k_order_not_found));
    EXPECT_TRUE(is_settled_order_refusal(k_already_closed));
    EXPECT_TRUE(is_settled_order_refusal(k_already_filled));
    EXPECT_TRUE(is_settled_order_refusal(k_not_open_order));
    EXPECT_TRUE(is_settled_order_refusal(k_edit_replaced));
    // a locked account is not settled
    EXPECT_FALSE(is_settled_order_refusal(k_account_locked));
    EXPECT_FALSE(is_settled_order_refusal(k_not_enough_funds));
}

TEST(DeribitErrors, LiveRefusalsParseAsTheEnvelopeTheyArriveIn) {
    const auto unauthorized = fixture("rest_error_unauthorized.json");
    const auto refused = parse_response(unauthorized);
    ASSERT_TRUE(refused.has_value());
    ASSERT_TRUE(refused->failed());
    EXPECT_EQ(refused->error_code, k_unauthorized);
    EXPECT_EQ(refused->error_message, "unauthorized");
    EXPECT_EQ(classify(refused->error_code), exec::ErrorClass::AuthFailure);

    // negative json-rpc codes share the field, so no-error is not 0
    const auto missing = fixture("rest_error_method_not_found.json");
    const auto not_found = parse_response(missing);
    ASSERT_TRUE(not_found.has_value());
    ASSERT_TRUE(not_found->failed());
    EXPECT_EQ(not_found->error_code, k_method_not_found);
    EXPECT_NE(k_no_error, 0);

    const auto ok = fixture("rest_private_buy_ack.json");
    const auto accepted = parse_response(ok);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_FALSE(accepted->failed());
}

} // namespace
} // namespace raikiri::venues::deribit
