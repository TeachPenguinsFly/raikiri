#include <raikiri/net/signer.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::mexc {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
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

// The real catalogue, loaded through the real parser and mapper: a table built
// by hand would agree with whatever the mapper happens to do.
exec::InstrumentTable catalogue() {
    const auto body = fixture("rest_contract_detail.json");
    static thread_local InstrumentBuffer storage;
    const auto wire = parse_instruments(body, storage);
    EXPECT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    table.reserve(wire->size()); // the table's keys are views into its own storage
    for (const auto& entry : *wire) {
        if (auto instrument = map_instrument(entry, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

// -------------------------------------------------------------- venue ----

TEST(MexcVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Mexc, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Mexc);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(MexcVenue, AnUnbuildableCommandIsRefusedLocally) {
    // No catalogue and no credentials, so nothing can be built. Silence would
    // leave the order open in the store forever.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Mexc, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Mexc, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

TEST(MexcVenue, SubscribingAnUnknownInstrumentIsRefused) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Mexc, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    const auto unknown = InstrumentId::make(VenueId::Mexc, MarketType::Perp, SymbolId{7});
    const auto result = adapter->data->subscribe(exec::SubKind::Bbo, unknown);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().cls, exec::ErrorClass::Terminal);
}

TEST(MexcVenue, CapsRowAgreesWithWhatTheAdapterCanDo) {
    const auto& caps = exec::caps_of(VenueId::Mexc);
    EXPECT_GE(caps.max_client_order_id_len, exec::k_min_client_order_id_len);
    EXPECT_FALSE(caps.ws_order_entry) << "orders go over REST; the socket is market data only";
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::AuthSubscribe);
    EXPECT_FALSE(caps.book_checksum) << "the depth channel guarantees begin/end, not a checksum";
}

// --------------------------------------------------------------- wire ----

TEST(MexcWire, EventDispatchResolvesChannelAndSymbol) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker.json", EventKind::Ticker},
                                     std::pair{"ws_deal.json", EventKind::Trade},
                                     std::pair{"ws_depth_update.json", EventKind::Depth},
                                     std::pair{"ws_depth_full.json", EventKind::DepthFull}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        // The symbol is in the ENVELOPE, not in the payload.
        EXPECT_EQ(event->symbol, "BTC_USDT") << file;
    }
}

TEST(MexcWire, DepthAndDepthFullAreNotConfused) {
    // "push.depth" is a prefix of "push.depth.full" and the two carry opposite
    // guarantees: a prefix match would let an unordered delta be read as a
    // top-of-book snapshot.
    const auto delta = fixture("ws_depth_update.json");
    const auto snapshot = fixture("ws_depth_full.json");
    EXPECT_EQ(parse_event(delta)->kind, EventKind::Depth);
    EXPECT_EQ(parse_event(snapshot)->kind, EventKind::DepthFull);
}

TEST(MexcWire, ControlFramesAreNotMistakenForData) {
    // An acknowledgement and a refusal differ only in the channel name.
    // Reading a refusal as an ack turns "the venue rejected this" into "this is
    // quiet", which is indistinguishable from a market with no activity.
    const auto ack = fixture("ws_subscribe_ack.json");
    const auto pong = fixture("ws_pong.json");
    EXPECT_EQ(parse_event(ack)->kind, EventKind::SubscribeAck);
    EXPECT_EQ(parse_event(pong)->kind, EventKind::Pong);

    const auto bad = fixture("ws_error.json");
    ASSERT_EQ(parse_event(bad)->kind, EventKind::Error);
    const auto err = parse_stream_error(bad);
    ASSERT_TRUE(err.has_value());
    // No code exists to classify on: the reason is prose.
    EXPECT_EQ(err->msg, "Contract [NOSUCH_USDT] not exists");
}

TEST(MexcWire, IdleTimeoutArrivesOnTheSameErrorChannelAsARefusal) {
    // Captured by holding a socket open for 100 s without sending anything
    // while it kept RECEIVING pushes. Inbound traffic does not reset MEXC's
    // timer, which is the whole reason poll() sends an application ping.
    const auto frame = fixture("ws_idle_timeout.json");
    ASSERT_EQ(parse_event(frame)->kind, EventKind::Error);
    const auto err = parse_stream_error(frame);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->msg, "more than 60 seconds no response, close the channel");
}

TEST(MexcWire, TickerCarriesPricesAsNumberTokens) {
    const auto frame = fixture("ws_ticker.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->symbol, "BTC_USDT");
    // Unquoted JSON numbers, handed on as their literal tokens so the decimal
    // parser sees exactly what MEXC wrote and no double is built on the way.
    EXPECT_EQ(ticker->bid_price, "64155");
    EXPECT_EQ(ticker->ask_price, "64155.1");
    EXPECT_EQ(ticker->fair_price, "64150.4");
    EXPECT_EQ(ticker->funding_rate, "0.000085");
    EXPECT_EQ(ticker->ts_ms, 1'786'394'656'908ULL);

    // Views point into the caller's buffer, not into parser scratch.
    EXPECT_GE(ticker->bid_price.data(), frame.data());
    EXPECT_LE(ticker->bid_price.data() + ticker->bid_price.size(), frame.data() + frame.size());
}

TEST(MexcWire, DealFrameKeepsEveryPrintAndItsDirection) {
    const auto frame = fixture("ws_deal.json");
    TradeBuffer storage;
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 4U);
    // Per-print objects carry no symbol; it comes off the envelope.
    EXPECT_EQ((*trades)[0].symbol, "BTC_USDT");
    EXPECT_EQ((*trades)[0].price, "64153.7");
    EXPECT_EQ((*trades)[0].qty, "544");
    EXPECT_EQ((*trades)[0].trade_id, "15678178824");
    EXPECT_EQ((*trades)[0].ts_ms, 1'786'394'659'519ULL);
    EXPECT_TRUE((*trades)[0].aggressor_bought);  // T:1
    EXPECT_FALSE((*trades)[3].aggressor_bought); // T:2
}

TEST(MexcWire, DepthDeltasChainOnBeginAndEnd) {
    // Two CONSECUTIVE frames off one live socket, so the chain across them is
    // real rather than constructed.
    LevelBuffer storage;
    const auto first = fixture("ws_depth_update.json");
    const auto second = fixture("ws_depth_update_next.json");
    const auto a = parse_depth(first, storage);
    ASSERT_TRUE(a.has_value());
    EXPECT_FALSE(a->snapshot);
    EXPECT_EQ(a->begin, 40'611'837'158);
    EXPECT_EQ(a->end, 40'611'837'162);
    EXPECT_EQ(a->version, a->end);
    const auto prev_end = a->end;

    const auto b = parse_depth(second, storage);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->begin, prev_end + 1);
}

TEST(MexcWire, DepthFullIsASnapshotWithSizes) {
    LevelBuffer storage;
    const auto frame = fixture("ws_depth_full.json");
    const auto depth = parse_depth(frame, storage);
    ASSERT_TRUE(depth.has_value());
    EXPECT_TRUE(depth->snapshot);
    ASSERT_EQ(depth->bids.size(), k_book_depth);
    ASSERT_EQ(depth->asks.size(), k_book_depth);
    // [price, contracts, resting orders]; the third element is dropped.
    EXPECT_EQ(depth->bids.front().price, "64124.8");
    EXPECT_EQ(depth->bids.front().qty, "399964");
    EXPECT_EQ(depth->asks.front().price, "64124.9");
    // `cts` is the matching-engine time, not the envelope's send time.
    EXPECT_EQ(depth->ts_ms, 1'786'982'679'141ULL);
}

TEST(MexcWire, RestDepthHasNoChannelAndFallsBackToTheEnvelopeClock) {
    // The REST body is a snapshot by construction and its `cts` is null, so
    // the only clock it has is `timestamp`. A zero here would poison every
    // latency histogram downstream.
    LevelBuffer storage;
    const auto body = fixture("rest_depth.json");
    const auto depth = parse_depth(body, storage);
    ASSERT_TRUE(depth.has_value());
    EXPECT_TRUE(depth->snapshot);
    EXPECT_EQ(depth->ts_ms, 1'786'394'654'631ULL);
    EXPECT_EQ(depth->bids.front().price, "64150.4");
}

TEST(MexcWire, RestEnvelopeSeparatesSuccessFromFailure) {
    const auto time_body = fixture("rest_server_time.json");
    const auto ok = parse_rest_envelope(time_body);
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok->success);
    EXPECT_EQ(ok->code, k_ok);

    for (const auto& [file, code] : {std::pair{"rest_error_bad_contract.json", 1001},
                                     std::pair{"rest_error_bad_parameter.json", 600}}) {
        const auto body = fixture(file);
        const auto bad = parse_rest_envelope(body);
        ASSERT_TRUE(bad.has_value()) << file;
        EXPECT_FALSE(bad->success) << file;
        EXPECT_EQ(bad->code, code) << file;
        EXPECT_FALSE(bad->msg.empty()) << file;
    }
}

TEST(MexcWire, EnvelopeThatCannotBeReadIsNotSuccess) {
    // Zero is the success code, so an unreadable body must fail rather than
    // decay into one that said zero.
    EXPECT_FALSE(parse_rest_envelope(R"({"code":0})").has_value());
    EXPECT_FALSE(parse_rest_envelope("not json").has_value());
}

TEST(MexcWire, ServerTimeIsTheDataFieldItself) {
    // `data` is the epoch-millisecond number, not an object or an array.
    const auto body = fixture("rest_server_time.json");
    const auto ms = parse_server_time(body);
    ASSERT_TRUE(ms.has_value());
    EXPECT_EQ(*ms, 1'786'394'654'676ULL);
}

TEST(MexcWire, InstrumentRowsKeepScalesRatherThanSteps) {
    const auto body = fixture("rest_contract_detail.json");
    InstrumentBuffer storage;
    const auto rows = parse_instruments(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].symbol, "BTC_USDT");
    EXPECT_EQ((*rows)[0].price_scale, 1);
    EXPECT_EQ((*rows)[0].vol_scale, 0);
    EXPECT_EQ((*rows)[0].contract_size, "0.0001");
    EXPECT_EQ((*rows)[0].state, 0);
    EXPECT_TRUE((*rows)[0].api_allowed);
    EXPECT_FALSE((*rows)[2].api_allowed) << "AIINU_USDT is data-only on this venue";
    EXPECT_EQ((*rows)[3].price_scale, 11) << "SATS_USDT is finer than FixedE9 models";
}

TEST(MexcWire, RestFundingRateCarriesTheSchedule) {
    const auto body = fixture("rest_funding_rate.json");
    const auto funding = parse_funding_rate(body);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->symbol, "BTC_USDT");
    EXPECT_EQ(funding->funding_rate, "0.000085");
    EXPECT_EQ(funding->fair_price, "64150.4");
    EXPECT_EQ(funding->collect_cycle_h, 8);
    EXPECT_EQ(funding->next_settle_ms, 1'786'406'400'000ULL);
}

// ---------------------------------------------------------------- map ----

TEST(MexcMap, StepIsDerivedAsAnIntegerPowerOfTen) {
    // MEXC states a digit count, never a step, so this derivation is ours.
    EXPECT_EQ(step_from_scale(0)->raw, k_e9);
    EXPECT_EQ(step_from_scale(1)->raw, 100'000'000);
    EXPECT_EQ(step_from_scale(9)->raw, 1);
    EXPECT_FALSE(step_from_scale(10).has_value());
    EXPECT_FALSE(step_from_scale(-1).has_value());
}

TEST(MexcMap, InstrumentDerivesStepsAndCarriesContractSize) {
    const auto table = catalogue();
    // SATS_USDT is refused at priceScale 11, so three of the four rows load.
    EXPECT_EQ(table.size(), 3U);

    const auto* btc = table.find("BTC_USDT");
    ASSERT_NE(btc, nullptr);
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.tick_size.raw, 100'000'000);  // 10^-1
    EXPECT_EQ(base.lot_size.raw, k_e9);          // volScale 0: one contract
    EXPECT_EQ(base.contract_value.raw, 100'000); // 0.0001 BTC a contract
    EXPECT_TRUE(base.min_notional.is_undef()) << "minVol is contracts, not a notional";
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_interval_s, 0U)
        << "/contract/detail carries no funding field at all";
}

TEST(MexcMap, ScaleFinerThanFixedPointIsRefusedNotRounded) {
    // SATS_USDT lists priceScale 11 and FixedE9 stops at nine fractional
    // digits. Loading it with a rounded tick would make every later price
    // conversion silently wrong.
    WireInstrument wire{};
    wire.symbol = "SATS_USDT";
    wire.state = 0;
    wire.price_scale = 11;
    wire.vol_scale = 0;
    wire.contract_size = "10000000";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(MexcMap, ApiDisallowedRowStillLoadsForMarketData) {
    // apiAllowed gates ORDER placement. Filtering on it would blind the
    // gateway to contracts it can legitimately stream.
    const auto table = catalogue();
    EXPECT_NE(table.find("AIINU_USDT"), nullptr);
}

TEST(MexcMap, SpotIsRefusedBecauseTheEndpointIsFuturesOnly) {
    // Answering a spot request from futures rows is a whole category of
    // position bug.
    WireInstrument wire{};
    wire.symbol = "BTC_USDT";
    wire.state = 0;
    wire.price_scale = 1;
    wire.vol_scale = 0;
    wire.contract_size = "0.0001";
    EXPECT_TRUE(map_instrument(wire, MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(wire, MarketType::Spot).has_value());
    wire.state = 1;
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value()) << "state 0 is enabled";
}

TEST(MexcMap, BboComesFromTheSnapshotChannel) {
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = fixture("ws_depth_full.json");
    const auto depth = parse_depth(frame, storage);
    ASSERT_TRUE(depth.has_value());

    const auto bbo = map_bbo(*depth, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 641'248); // 64124.8 / 0.1
    EXPECT_EQ(bbo->ask.raw, 641'249);
    EXPECT_EQ(bbo->bid_qty.raw, 399'964); // contracts, lot step 1
    EXPECT_EQ(bbo->ask_qty.raw, 514'117);
    EXPECT_EQ(bbo->ts_event.ns, 1'786'982'679'141ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(MexcMap, IncrementalDepthNeverBecomesABbo) {
    // The captured delta leads with a bid 1900 below the touch. Treating its
    // first entry as top of book would publish a spread that never existed.
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = fixture("ws_depth_update.json");
    const auto depth = parse_depth(frame, storage);
    ASSERT_TRUE(depth.has_value());
    ASSERT_FALSE(depth->snapshot);
    EXPECT_EQ(depth->bids.front().price, "62229.0");
    EXPECT_FALSE(map_bbo(*depth, table, k_ingest).has_value());
}

TEST(MexcMap, BookSnapshotCrossesIntoTheDomain) {
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = fixture("ws_depth_full.json");
    const auto depth = parse_depth(frame, storage);
    ASSERT_TRUE(depth.has_value());

    const auto book = map_book(*depth, table, k_ingest);
    ASSERT_TRUE(book.has_value());
    // Every level the subscription asked for reaches the domain snapshot; a
    // book that stops at five is the old tier still being served.
    EXPECT_EQ(book->bid_count, k_book_depth);
    EXPECT_EQ(book->ask_count, k_book_depth);
    static_assert(k_book_depth <= k_book_levels);
    EXPECT_EQ(book->venue_seq, 40'789'901'950ULL);
    EXPECT_EQ(book->ts_event.ns, 1'786'982'679'141ULL * 1'000'000ULL);

    // Bids descend, asks ascend, and the two never cross — a book that fails
    // this is not a book, and publishing it would misprice every strategy
    // reading it.
    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw);
    for (std::uint32_t i = 1; i < book->bid_count; ++i) {
        EXPECT_LT(book->bids[i].price.raw, book->bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].price.raw, book->asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < book->bid_count; ++i) {
        EXPECT_GT(book->bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].qty.raw, 0) << i;
    }
}

TEST(MexcMap, IncrementalDepthNeverBecomesABook) {
    // The delta carries only what changed, so publishing it as a snapshot
    // would hand shm a five-level book with a bid 1900 off the touch.
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = fixture("ws_depth_update.json");
    const auto depth = parse_depth(frame, storage);
    ASSERT_TRUE(depth.has_value());
    EXPECT_FALSE(map_book(*depth, table, k_ingest).has_value());
}

TEST(MexcMap, UnknownSymbolYieldsNoBook) {
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = fixture("ws_depth_full.json");
    auto depth = parse_depth(frame, storage);
    ASSERT_TRUE(depth.has_value());
    depth->symbol = "SATS_USDT"; // refused at load, so no id exists for it
    EXPECT_FALSE(map_book(*depth, table, k_ingest).has_value());
}

TEST(MexcMap, PriceOffTheTickIsDroppedNotRounded) {
    // A price that does not divide by the tick means MEXC changed an
    // instrument's precision under us. Rounding would put an order one tick
    // from where it was meant.
    const auto table = catalogue();
    const std::array<WireLevel, 1> bids{WireLevel{.price = "64142.45", .qty = "1"}};
    const std::array<WireLevel, 1> asks{WireLevel{.price = "64142.5", .qty = "1"}};
    WireDepth wire{};
    wire.symbol = "BTC_USDT";
    wire.snapshot = true;
    wire.bids = bids;
    wire.asks = asks;
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(MexcMap, UnknownSymbolIsDropped) {
    const auto table = catalogue();
    WireTrade wire{};
    wire.symbol = "SATS_USDT"; // refused at load, so no id exists for it
    wire.price = "1";
    wire.qty = "1";
    EXPECT_FALSE(map_trade(wire, table, k_ingest).has_value());
}

TEST(MexcMap, TradeAggressorIsNotInverted) {
    const auto table = catalogue();
    TradeBuffer storage;
    const auto frame = fixture("ws_deal.json");
    const auto wire = parse_trades(frame, storage);
    ASSERT_TRUE(wire.has_value());

    const auto buy = map_trade((*wire)[0], table, k_ingest);
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->price.raw, 641'537); // 64153.7 / 0.1
    EXPECT_EQ(buy->qty.raw, 544);       // contracts
    EXPECT_EQ(buy->aggressor, Side::Buy);
    EXPECT_EQ(buy->ts_event.ns, 1'786'394'659'519ULL * 1'000'000ULL);

    const auto sell = map_trade((*wire)[3], table, k_ingest);
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->aggressor, Side::Sell);
}

TEST(MexcMap, FundingFromTheTickerHasNoSchedule) {
    const auto table = catalogue();
    const auto frame = fixture("ws_ticker.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());

    const auto funding = map_funding(*ticker, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 85'000);        // 0.000085
    EXPECT_EQ(funding->mark_price.raw, 641'504); // fairPrice is MEXC's mark
    // The channel states no settlement boundary. A zero reads as unknown; a
    // guessed 8h would read as fact.
    EXPECT_EQ(funding->next_funding_time.ns, 0U);
    EXPECT_EQ(funding->interval_s, 0U);
    EXPECT_EQ(funding->ts_event.ns, 1'786'394'656'908ULL * 1'000'000ULL);
}

TEST(MexcMap, FundingFromRestCarriesTheSchedule) {
    const auto table = catalogue();
    const auto body = fixture("rest_funding_rate.json");
    const auto wire = parse_funding_rate(body);
    ASSERT_TRUE(wire.has_value());

    const auto funding = map_funding(*wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 85'000);
    EXPECT_EQ(funding->interval_s, 8U * 3600U); // collectCycle is HOURS
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'406'400'000ULL * 1'000'000ULL);
}

TEST(MexcMap, ContractWithoutAFundingRateIsNotFundingAtZero) {
    const auto table = catalogue();
    WireTicker wire{};
    wire.symbol = "BTC_USDT";
    wire.fair_price = "64150.4";
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

// ------------------------------------------------------------- errors ----

TEST(MexcErrors, VerifiedCodesLandInTheRightBuckets) {
    EXPECT_EQ(classify(k_rate_limited), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_not_logged_in), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_contract_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_parameter_error), exec::ErrorClass::Terminal);
}

TEST(MexcErrors, ClockSkewIsRetryableNotAuthFailure) {
    // The same payload signed against a corrected offset succeeds, so filing
    // this under AuthFailure would halt a venue whose keys are fine.
    EXPECT_EQ(classify(k_request_expired), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_internal_error), exec::ErrorClass::Retryable);
}

TEST(MexcErrors, UnknownCodeIsTerminal) {
    // An unknown refusal repeated at full speed is how an account earns an IP
    // ban; a spuriously terminal order is recoverable by reconciliation.
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
}

// ----------------------------------------------------------------- ws ----

TEST(MexcWs, SubscribeFrameNamesExactlyOneSymbol) {
    Protocol protocol;
    const std::array<Subscription, 1> one{Subscription{.channel = "ticker", .symbol = "BTC_USDT"}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"method":"sub.ticker","param":{"symbol":"BTC_USDT"}})");
    // There is no batch form: `param` takes one symbol, so a wide gateway
    // sends one message per symbol per channel.
    EXPECT_EQ(frame->consumed, 1U);
}

TEST(MexcWs, DepthFullCarriesItsLevelCountInTheSubscription) {
    Protocol protocol;
    const std::array<Subscription, 1> one{
        Subscription{.channel = "depth.full", .symbol = "BTC_USDT", .limit = k_book_depth}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text,
              R"({"method":"sub.depth.full","param":{"symbol":"BTC_USDT","limit":50}})");
}

TEST(MexcWs, UnsubscribeReusesTheChannelStem) {
    Protocol protocol;
    const std::array<Subscription, 1> one{Subscription{.channel = "deal", .symbol = "ETH_USDT"}};
    const auto frame = protocol.unsubscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"method":"unsub.deal","param":{"symbol":"ETH_USDT"}})");
}

TEST(MexcWs, ChannelNamesMatchWhatTheSocketPushesBack) {
    // sub.<stem> is answered by rs.sub.<stem> and delivers push.<stem>; the
    // parser keys on the push form, so the two must not drift.
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "depth.full");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "deal");
    EXPECT_EQ(channel_for(exec::SubKind::Depth), "depth");
    EXPECT_EQ(channel_for(exec::SubKind::Funding), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice), "ticker");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());
    EXPECT_EQ(Protocol::ping_frame(), R"({"method":"ping"})");
}

TEST(MexcWs, AKindMexcDoesNotStreamIsRefusedRatherThanSentEmpty) {
    // MEXC ignores an unknown method in silence, so a malformed frame would
    // present as a channel that simply never delivers.
    Protocol protocol;
    const std::array<Subscription, 1> one{Subscription{.channel = {}, .symbol = "BTC_USDT"}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_EQ(protocol.subscribe_frame({}).error(), WsBuildError::NothingToSend);
}

// --------------------------------------------------------------- rest ----

TEST(MexcRest, SymbolGoesInThePathForDepthAndFundingButInTheQueryForTicker) {
    // The two shapes coexist on the same API; sending one as the other is a
    // 1001 that reads like a delisted contract.
    RequestBuilder builder;
    EXPECT_EQ(builder.depth("BTC_USDT", 5).target, "/api/v1/contract/depth/BTC_USDT?limit=5");
    EXPECT_EQ(builder.funding_rate("BTC_USDT").target, "/api/v1/contract/funding_rate/BTC_USDT");
    EXPECT_EQ(builder.ticker("BTC_USDT").target, "/api/v1/contract/ticker?symbol=BTC_USDT");
    EXPECT_EQ(builder.contract_detail().target, "/api/v1/contract/detail");
    EXPECT_EQ(builder.server_time().target, "/api/v1/contract/ping");
    EXPECT_EQ(rest_host(), "contract.mexc.com");
    EXPECT_EQ(stream_host(MarketType::Perp), "contract.mexc.com");
    EXPECT_EQ(stream_path(MarketType::Perp), "/edge");
    // Public data and signed calls live on different names: contract.mexc.com
    // 403s a private POST at the CDN, api.mexc.com answers it.
    EXPECT_EQ(private_rest_host(), "api.mexc.com");
    EXPECT_NE(private_rest_host(), rest_host());
}

// ------------------------------------------------- spot market data ----

exec::InstrumentTable spot_catalogue() {
    static thread_local SpotSymbolBuffer storage;
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_spot_symbols(body, storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (const auto instrument = map_spot_instrument(row); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

TEST(MexcSpotStream, TheEnvelopeNamesTheFrameByFieldNumber) {
    // Not by the channel string: the number is what the decoder can act on,
    // and an unknown one is skipped rather than guessed at.
    const auto quote = fixture("ws_spot_book_ticker.pb");
    const auto event = parse_stream_event(quote);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, StreamKind::Quote);
    EXPECT_EQ(event->symbol, "BTCUSDT");
    EXPECT_GT(event->ts_ms, 1'000'000'000'000ULL);
    EXPECT_FALSE(event->payload.empty());

    // JSON on the same socket is not a message, and must not read as one.
    EXPECT_FALSE(parse_stream_event(R"({"id":0,"code":0,"msg":"PONG"})").has_value());
}

TEST(MexcSpotStream, TheQuoteDecodesToAnUncrossedBook) {
    const auto frame = fixture("ws_spot_book_ticker.pb");
    const auto event = parse_stream_event(frame);
    ASSERT_TRUE(event.has_value());
    const auto quote = parse_stream_quote(event->payload);
    ASSERT_TRUE(quote.has_value());

    const auto table = spot_catalogue();
    const auto bbo = map_spot_bbo(*quote, event->symbol, event->ts_ms, table, UnixNanos{9});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->instrument.market(), MarketType::Spot);
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
    EXPECT_GT(bbo->bid_qty.raw, 0);
}

TEST(MexcSpotStream, EveryPrintNamesItsTaker) {
    // tradeType 1 is the taker BUYING — verified against GET /api/v3/trades,
    // which states isBuyerMaker outright.
    const auto frame = fixture("ws_spot_deals.pb");
    const auto event = parse_stream_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, StreamKind::Trade);

    StreamTradeBuffer storage;
    const auto trades = parse_stream_trades(event->payload, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_FALSE(trades->empty());
    const auto table = spot_catalogue();
    for (const auto& print : *trades) {
        EXPECT_FALSE(print.price.empty());
        EXPECT_NE(print.ts_ms, 0U);
        const auto tick = map_spot_trade(print, event->symbol, table, UnixNanos{9});
        ASSERT_TRUE(tick.has_value());
        EXPECT_EQ(tick->aggressor, print.aggressor_bought ? Side::Buy : Side::Sell);
    }
}

TEST(MexcSpotStream, TheBookIsWholeAndItsSizesAreFinerThanTheLot) {
    // MEXC quotes sizes at baseAssetPrecision (1e-8) and trades at
    // baseSizePrecision (1e-6). An exact conversion refuses nearly every
    // level, which would void the whole book — so market data truncates, and
    // the discarded remainder is below the smallest tradable size.
    const auto frame = fixture("ws_spot_depth.pb");
    const auto event = parse_stream_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, StreamKind::Depth);

    LevelBuffer storage;
    const auto book = parse_stream_book(event->payload, storage);
    ASSERT_TRUE(book.has_value());
    ASSERT_FALSE(book->bids.empty());
    ASSERT_FALSE(book->asks.empty());

    const auto table = spot_catalogue();
    const auto depth = map_spot_book(*book, event->symbol, event->ts_ms, table, UnixNanos{9});
    ASSERT_TRUE(depth.has_value());
    EXPECT_GT(depth->bid_count, 0U);
    EXPECT_GT(depth->ask_count, 0U);
    EXPECT_LT(depth->bids[0].price.raw, depth->asks[0].price.raw) << "an uncrossed book";
    EXPECT_GT(depth->bids[0].price.raw, depth->bids[1].price.raw) << "bids best first";
    EXPECT_LT(depth->asks[0].price.raw, depth->asks[1].price.raw) << "asks best first";
}

} // namespace
} // namespace raikiri::venues::mexc
