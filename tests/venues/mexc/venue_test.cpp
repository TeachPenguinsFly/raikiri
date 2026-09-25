#include <raikiri/domain/asset_class.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/mexc/account.hpp>
#include <raikiri/venues/mexc/book.hpp>
#include <raikiri/venues/mexc/catalogue.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/market_data.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/positions.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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

// the real catalogue through the real parser and mapper
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

TEST(MexcWire, TheBulkFundingAnswerStatesEveryCycleTheVenueUses) {
    // the only place mexc states an interval; the body is held because the rows view it
    const auto body = fixture("rest_funding_rates.json");
    auto storage = std::make_unique<FundingRateBuffer>();
    const auto rows = parse_funding_rates(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    std::map<std::string, std::int64_t> cycle;
    for (const auto& row : *rows) {
        cycle[std::string{row.symbol}] = row.collect_cycle_h;
        EXPECT_FALSE(row.max_rate.empty()) << row.symbol;
        EXPECT_FALSE(row.min_rate.empty()) << row.symbol;
    }
    EXPECT_EQ(cycle["BTC_USDT"], 8);
    EXPECT_EQ(cycle["XAU_USDT"], 4);
    EXPECT_EQ(cycle["LSK_USDT"], 1);
    EXPECT_EQ(cycle["US30_USDT"], 24);
}

TEST(MexcVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue({.venue = VenueId::Mexc, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Mexc);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(MexcVenue, AnUnbuildableCommandIsRefusedLocally) {
    // no catalogue or credentials, so the refusal must arrive as an event
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

TEST(MexcWire, EventDispatchResolvesChannelAndSymbol) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker.json", EventKind::Ticker},
                                     std::pair{"ws_deal.json", EventKind::Trade},
                                     std::pair{"ws_depth_update.json", EventKind::Depth},
                                     std::pair{"ws_depth_full.json", EventKind::DepthFull}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        // the symbol is in the envelope
        EXPECT_EQ(event->symbol, "BTC_USDT") << file;
    }
}

TEST(MexcWire, OneOpenReadsTheSameEventAndBookAsTwo) {
    for (const char* file : {"ws_depth_full.json",
                             "ws_depth_update.json",
                             "ws_deal.json",
                             "ws_ticker.json",
                             "ws_subscribe_ack.json",
                             "ws_error.json"}) {
        const auto frame = fixture(file);
        LevelBuffer one;
        const auto both = parse_event_and_depth(frame, one);
        const auto event = parse_event(frame);
        ASSERT_TRUE(both.has_value()) << file;
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(both->event.kind, event->kind) << file;
        EXPECT_EQ(both->event.symbol, event->symbol) << file;
        EXPECT_EQ(both->depth.has_value(), event->kind == EventKind::DepthFull) << file;
        if (!both->depth.has_value()) {
            continue;
        }
        LevelBuffer two;
        const auto depth = parse_depth(frame, two);
        ASSERT_TRUE(depth.has_value()) << file;
        EXPECT_EQ(both->depth->symbol, depth->symbol);
        EXPECT_EQ(both->depth->snapshot, depth->snapshot);
        EXPECT_EQ(both->depth->version, depth->version);
        EXPECT_EQ(both->depth->begin, depth->begin);
        EXPECT_EQ(both->depth->end, depth->end);
        EXPECT_EQ(both->depth->ts_ms, depth->ts_ms);
        ASSERT_EQ(both->depth->bids.size(), depth->bids.size());
        ASSERT_EQ(both->depth->asks.size(), depth->asks.size());
        for (std::size_t i = 0; i < depth->bids.size(); ++i) {
            EXPECT_EQ(both->depth->bids[i].price, depth->bids[i].price) << i;
            EXPECT_EQ(both->depth->bids[i].qty, depth->bids[i].qty) << i;
        }
        for (std::size_t i = 0; i < depth->asks.size(); ++i) {
            EXPECT_EQ(both->depth->asks[i].price, depth->asks[i].price) << i;
            EXPECT_EQ(both->depth->asks[i].qty, depth->asks[i].qty) << i;
        }
    }
}

TEST(MexcWire, ASnapshotWithoutBeginOrEndReadsThemAsZero) {
    const auto frame = fixture("ws_depth_full.json");
    ASSERT_EQ(frame.find("\"begin\""), std::string::npos);
    ASSERT_EQ(frame.find("\"end\""), std::string::npos);
    LevelBuffer storage;
    const auto depth = parse_depth(frame, storage);
    ASSERT_TRUE(depth.has_value());
    EXPECT_TRUE(depth->snapshot);
    EXPECT_EQ(depth->begin, 0);
    EXPECT_EQ(depth->end, 0);
    EXPECT_EQ(depth->version, 40789901950);
    EXPECT_EQ(depth->ts_ms, 1786982679141U);
    EXPECT_FALSE(depth->bids.empty());
    EXPECT_FALSE(depth->asks.empty());
}

TEST(MexcWire, DepthAndDepthFullAreNotConfused) {
    // "push.depth" prefixes "push.depth.full", so prefix matching would read a delta as a snapshot
    const auto delta = fixture("ws_depth_update.json");
    const auto snapshot = fixture("ws_depth_full.json");
    EXPECT_EQ(parse_event(delta)->kind, EventKind::Depth);
    EXPECT_EQ(parse_event(snapshot)->kind, EventKind::DepthFull);
}

TEST(MexcWire, ControlFramesAreNotMistakenForData) {
    // an ack and a refusal differ only in channel name
    const auto ack = fixture("ws_subscribe_ack.json");
    const auto pong = fixture("ws_pong.json");
    EXPECT_EQ(parse_event(ack)->kind, EventKind::SubscribeAck);
    EXPECT_EQ(parse_event(pong)->kind, EventKind::Pong);

    const auto bad = fixture("ws_error.json");
    ASSERT_EQ(parse_event(bad)->kind, EventKind::Error);
    const auto err = parse_stream_error(bad);
    ASSERT_TRUE(err.has_value());
    // prose, no code
    EXPECT_EQ(err->msg, "Contract [NOSUCH_USDT] not exists");
}

TEST(MexcWire, IdleTimeoutArrivesOnTheSameErrorChannelAsARefusal) {
    // inbound pushes do not reset the venue's idle timer
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
    // unquoted numbers kept as literal tokens
    EXPECT_EQ(ticker->bid_price, "64155");
    EXPECT_EQ(ticker->ask_price, "64155.1");
    EXPECT_EQ(ticker->fair_price, "64150.4");
    EXPECT_EQ(ticker->funding_rate, "0.000085");
    EXPECT_EQ(ticker->ts_ms, 1'786'394'656'908ULL);
    // `amount24` is quote turnover; `volume24` contracts are not carried
    EXPECT_EQ(ticker->volume_24h_quote, "4912147390.01821");

    // views point into the caller's buffer
    EXPECT_GE(ticker->bid_price.data(), frame.data());
    EXPECT_LE(ticker->bid_price.data() + ticker->bid_price.size(), frame.data() + frame.size());
}

TEST(MexcWire, DealFrameKeepsEveryPrintAndItsDirection) {
    const auto frame = fixture("ws_deal.json");
    TradeBuffer storage;
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 4U);
    // the symbol comes off the envelope
    EXPECT_EQ((*trades)[0].symbol, "BTC_USDT");
    EXPECT_EQ((*trades)[0].price, "64153.7");
    EXPECT_EQ((*trades)[0].qty, "544");
    EXPECT_EQ((*trades)[0].trade_id, "15678178824");
    EXPECT_EQ((*trades)[0].ts_ms, 1'786'394'659'519ULL);
    EXPECT_TRUE((*trades)[0].aggressor_bought);  // T:1
    EXPECT_FALSE((*trades)[3].aggressor_bought); // T:2
}

TEST(MexcWire, DepthDeltasChainOnBeginAndEnd) {
    // two consecutive live frames
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
    // `cts` is matching-engine time
    EXPECT_EQ(depth->ts_ms, 1'786'982'679'141ULL);
}

TEST(MexcWire, RestDepthHasNoChannelAndFallsBackToTheEnvelopeClock) {
    // the rest body's `cts` is null, so `timestamp` is the clock
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

    for (const auto& [file, code] :
         {std::pair{"rest_error_bad_contract.json", 1001}, std::pair{"rest_error_bad_parameter.json", 600}}) {
        const auto body = fixture(file);
        const auto bad = parse_rest_envelope(body);
        ASSERT_TRUE(bad.has_value()) << file;
        EXPECT_FALSE(bad->success) << file;
        EXPECT_EQ(bad->code, code) << file;
        EXPECT_FALSE(bad->msg.empty()) << file;
    }
}

TEST(MexcWire, EnvelopeThatCannotBeReadIsNotSuccess) {
    // an unreadable body must not decay into code 0
    EXPECT_FALSE(parse_rest_envelope(R"({"code":0})").has_value());
    EXPECT_FALSE(parse_rest_envelope("not json").has_value());
}

TEST(MexcWire, ServerTimeIsTheDataFieldItself) {
    // `data` is the epoch-ms number
    const auto body = fixture("rest_server_time.json");
    const auto ms = parse_server_time(body);
    ASSERT_TRUE(ms.has_value());
    EXPECT_EQ(*ms, 1'786'394'654'676ULL);
}

TEST(MexcMap, TheConceptPlateStatesTheClass) {
    const auto body = fixture("rest_contract_detail_live.json");
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_instruments(body, storage);
    ASSERT_TRUE(rows.has_value());
    const auto classified = [&](std::string_view symbol) {
        for (const auto& row : *rows) {
            if (row.symbol != symbol) {
                continue;
            }
            auto instrument = map_instrument(row, MarketType::Perp);
            if (!instrument.has_value()) {
                break;
            }
            classify(VenueId::Mexc, *instrument);
            return base_of(*instrument);
        }
        ADD_FAILURE() << symbol;
        return InstrumentBase{};
    };
    const auto expect = [&](std::string_view symbol, AssetClass cls, std::string_view underlying) {
        const auto base = classified(symbol);
        EXPECT_EQ(base.asset_class, cls) << symbol;
        EXPECT_EQ(base.underlying.view(), underlying) << symbol;
    };
    expect("BTC_USDT", AssetClass::Crypto, "BTC");
    expect("SPX_USDT", AssetClass::Crypto, "SPX");
    expect("PONS_USDT", AssetClass::Crypto, "PONS");
    expect("PAXG_USDT", AssetClass::Crypto, "PAXG");
    expect("AAPLSTOCK_USDT", AssetClass::Equity, "AAPL");
    expect("SKHYNIXSTOCK_USDT", AssetClass::Equity, "SKHYNIX");
    expect("SONYSTOCK_USDT", AssetClass::Equity, "SONY");
    expect("OPENAI_USDT", AssetClass::Equity, "OPENAI");
    expect("TESLA_USDT", AssetClass::Equity, "TSLA");
    expect("COINBASE_USDT", AssetClass::Equity, "COIN");
    expect("SPY_USDT", AssetClass::Equity, "SPY");
    expect("DRAM_USDT", AssetClass::Equity, "DRAM");
    expect("NAS100_USDT", AssetClass::Index, "NDX");
    expect("US30_USDT", AssetClass::Index, "DJI");
    expect("XAU_USDT", AssetClass::Commodity, "XAU");
    expect("USOIL_USDT", AssetClass::Commodity, "CL");
    expect("EUR_USDT", AssetClass::FX, "EURUSD");
    expect("JPY_USDT", AssetClass::FX, "USDJPY");
}

TEST(MexcMap, LiveBooksOfTheSilentContractsQuote) {
    const auto body = fixture("rest_contract_detail_live.json");
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_instruments(body, storage);
    ASSERT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            ASSERT_TRUE(table.add(*instrument).has_value()) << row.symbol;
        }
    }
    EXPECT_NE(table.find(std::string_view{"NAS100_USDT"}), nullptr) << "contractSize 1e-05";
    for (const auto* file : {"ws_depth_full_nex.json", "ws_depth_full_floki.json", "ws_depth_full_zbt.json"}) {
        const auto frame = fixture(file);
        LevelBuffer levels;
        const auto parsed = parse_event_and_depth(frame, levels);
        ASSERT_TRUE(parsed.has_value()) << file;
        ASSERT_TRUE(parsed->depth.has_value()) << file;
        EXPECT_TRUE(map_bbo(*parsed->depth, table, k_ingest).has_value()) << file;
        EXPECT_TRUE(map_book(*parsed->depth, table, k_ingest).has_value()) << file;
    }
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

TEST(MexcMap, StepIsDerivedAsAnIntegerPowerOfTen) {
    // mexc states a digit count, never a step
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
    // priceScale 11 exceeds FixedE9's nine digits
    WireInstrument wire{};
    wire.symbol = "SATS_USDT";
    wire.base_coin = "SATS";
    wire.quote_coin = "USDT";
    wire.state = 0;
    wire.price_scale = 11;
    wire.vol_scale = 0;
    wire.contract_size = "10000000";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(MexcMap, ApiDisallowedRowStillLoadsForMarketData) {
    // apiAllowed gates order placement only
    const auto table = catalogue();
    EXPECT_NE(table.find("AIINU_USDT"), nullptr);
}

TEST(MexcMap, SpotIsRefusedBecauseTheEndpointIsFuturesOnly) {
    // futures rows must not answer a spot request
    WireInstrument wire{};
    wire.symbol = "BTC_USDT";
    wire.base_coin = "BTC";
    wire.quote_coin = "USDT";
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
    // the delta leads with a bid 1900 below the touch
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
    // every requested level reaches the snapshot
    EXPECT_EQ(book->bid_count, k_book_depth);
    EXPECT_EQ(book->ask_count, k_book_depth);
    static_assert(k_book_depth <= k_book_levels);
    EXPECT_EQ(book->venue_seq, 40'789'901'950ULL);
    EXPECT_EQ(book->ts_event.ns, 1'786'982'679'141ULL * 1'000'000ULL);

    // bids descend, asks ascend, and the sides never cross
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
    // a delta must not publish as a snapshot
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
    // an off-tick price is refused, not rounded
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
    // zero reads as unknown
    EXPECT_EQ(funding->next_funding_time.ns, 0U);
    EXPECT_EQ(funding->interval_s, 0U);
    EXPECT_EQ(funding->ts_event.ns, 1'786'394'656'908ULL * 1'000'000ULL);
}

TEST(MexcMap, FundingFromTheTickerCarriesTheCatalogueInterval) {
    const auto body = fixture("rest_contract_detail.json");
    static thread_local InstrumentBuffer storage;
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    table.reserve(wire->size());
    for (const auto& entry : *wire) {
        if (auto instrument = map_instrument(entry, MarketType::Perp); instrument.has_value()) {
            std::get<PerpInstrument>(*instrument).funding_interval_s = 4U * 3600U;
            ASSERT_TRUE(table.add(*instrument).has_value());
        }
    }
    const auto frame = fixture("ws_ticker.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());

    const auto funding = map_funding(*ticker, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->interval_s, 4U * 3600U);
    // the boundary rest_funding_rate.json states as nextSettleTime
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'406'400'000ULL * 1'000'000ULL);
}

TEST(MexcMap, TickerStatsScaleContractsIntoBaseUnits) {
    const auto table = catalogue();
    const auto frame = fixture("ws_ticker.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());

    const auto stats = map_ticker_stats(*ticker, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote.raw, 4'912'147'390'018'210LL);
    // 758957590 contracts of 0.0001 BTC.
    EXPECT_EQ(stats->volume_24h_base.raw, 75'895'759'000'000LL);
    EXPECT_EQ(stats->ts_event.ns, 1'786'394'656'908ULL * 1'000'000ULL);

    // a frame with no turnover publishes nothing
    WireTicker silent{};
    silent.symbol = "BTC_USDT";
    silent.funding_rate = "0.000085";
    EXPECT_FALSE(map_ticker_stats(silent, table, k_ingest).has_value());
}

TEST(MexcMap, FundingFromRestCarriesTheSchedule) {
    const auto table = catalogue();
    const auto body = fixture("rest_funding_rate.json");
    const auto wire = parse_funding_rate(body);
    ASSERT_TRUE(wire.has_value());

    const auto funding = map_funding(*wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 85'000);
    EXPECT_EQ(funding->interval_s, 8U * 3600U); // collectCycle is in hours
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'406'400'000ULL * 1'000'000ULL);
}

TEST(MexcMap, CoinMarginedContractIsInverseAndTurnsOverInDollars) {
    const auto body = fixture("rest_contract_detail_inverse.json");
    static thread_local InstrumentBuffer storage;
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    EXPECT_EQ(wire->front().settle_coin, "BTC");
    exec::InstrumentTable table;
    table.reserve(1);
    const auto instrument = map_instrument(wire->front(), MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).kind, ContractKind::Inverse);
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 100 * k_e9); // dollars a contract
    ASSERT_TRUE(table.add(*instrument).has_value());

    const auto frame = fixture("ws_ticker_inverse.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());
    const auto stats = map_ticker_stats(*ticker, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote.raw, 7'599'700'000'000LL); // 75997 contracts of $100
    EXPECT_EQ(stats->volume_24h_base.raw, 90'676'765'443LL);     // amount24 is btc here

    const auto linear = catalogue();
    EXPECT_EQ(base_of(*linear.find("BTC_USDT")).kind, ContractKind::Linear);
}

TEST(MexcMap, ContractWithoutAFundingRateIsNotFundingAtZero) {
    const auto table = catalogue();
    WireTicker wire{};
    wire.symbol = "BTC_USDT";
    wire.fair_price = "64150.4";
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

TEST(MexcErrors, VerifiedCodesLandInTheRightBuckets) {
    EXPECT_EQ(classify(k_rate_limited), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_not_logged_in), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_contract_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_parameter_error), exec::ErrorClass::Terminal);
}

TEST(MexcErrors, ClockSkewIsRetryableNotAuthFailure) {
    // retryable: the same payload signed with a corrected offset succeeds
    EXPECT_EQ(classify(k_request_expired), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_internal_error), exec::ErrorClass::Retryable);
}

TEST(MexcErrors, UnknownCodeIsTerminal) {
    // unknown codes are terminal
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
}

TEST(MexcWs, SubscribeFrameNamesExactlyOneSymbol) {
    Protocol protocol;
    const std::array<Subscription, 1> one{Subscription{.channel = "ticker", .symbol = "BTC_USDT"}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"method":"sub.ticker","param":{"symbol":"BTC_USDT"}})");
    // `param` takes one symbol; there is no batch form
    EXPECT_EQ(frame->consumed, 1U);
}

TEST(MexcWs, DepthFullCarriesItsLevelCountInTheSubscription) {
    Protocol protocol;
    const std::array<Subscription, 1> one{
        Subscription{.channel = "depth.full", .symbol = "BTC_USDT", .limit = k_book_depth}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"method":"sub.depth.full","param":{"symbol":"BTC_USDT","limit":50}})");
}

TEST(MexcWs, UnsubscribeReusesTheChannelStem) {
    Protocol protocol;
    const std::array<Subscription, 1> one{Subscription{.channel = "deal", .symbol = "ETH_USDT"}};
    const auto frame = protocol.unsubscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"method":"unsub.deal","param":{"symbol":"ETH_USDT"}})");
}

TEST(MexcWs, ChannelNamesMatchWhatTheSocketPushesBack) {
    // sub.<stem> is acked as rs.sub.<stem> and delivers push.<stem>
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "depth.full");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "deal");
    EXPECT_EQ(channel_for(exec::SubKind::Depth), "depth");
    EXPECT_EQ(channel_for(exec::SubKind::Funding), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice), "ticker");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());
    EXPECT_EQ(Protocol::ping_frame(), R"({"method":"ping"})");
}

TEST(MexcWs, AKindMexcDoesNotStreamIsRefusedRatherThanSentEmpty) {
    // mexc ignores an unknown method silently
    Protocol protocol;
    const std::array<Subscription, 1> one{Subscription{.channel = {}, .symbol = "BTC_USDT"}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_EQ(protocol.subscribe_frame({}).error(), WsBuildError::NothingToSend);
}

TEST(MexcRest, SymbolGoesInThePathForDepthAndFundingButInTheQueryForTicker) {
    // sending one shape as the other gets 1001
    RequestBuilder builder;
    EXPECT_EQ(builder.depth("BTC_USDT", 5).target, "/api/v1/contract/depth/BTC_USDT?limit=5");
    EXPECT_EQ(builder.funding_rate("BTC_USDT").target, "/api/v1/contract/funding_rate/BTC_USDT");
    EXPECT_EQ(builder.ticker("BTC_USDT").target, "/api/v1/contract/ticker?symbol=BTC_USDT");
    EXPECT_EQ(builder.contract_detail().target, "/api/v1/contract/detail");
    EXPECT_EQ(builder.server_time().target, "/api/v1/contract/ping");
    EXPECT_EQ(rest_host(), "contract.mexc.com");
    EXPECT_EQ(stream_host(MarketType::Perp), "contract.mexc.com");
    EXPECT_EQ(stream_path(MarketType::Perp), "/edge");
    // contract.mexc.com 403s a private post; api.mexc.com answers it
    EXPECT_EQ(private_rest_host(), "api.mexc.com");
    EXPECT_NE(private_rest_host(), rest_host());
}

TEST(MexcWire, FundingRowsSitUnderResultListWithTheirOwnPagingCounters) {
    static thread_local FundingRecordBuffer storage;
    const auto body = fixture("rest_funding_records.json");
    const auto rows = parse_funding_records(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].symbol, "BTC_USDT");
    EXPECT_EQ((*rows)[0].id, 328'033U);
    EXPECT_EQ((*rows)[0].position_type, 1);
    // bare json numbers kept as tokens, so trailing zeroes survive
    EXPECT_EQ((*rows)[0].funding, "0.0837798");
    EXPECT_EQ((*rows)[0].rate, "-0.002");
    EXPECT_EQ((*rows)[1].funding, "0.0125670");
    EXPECT_EQ((*rows)[0].settle_ms, 1'606'435'200'000ULL);
}

TEST(MexcMap, TheFundingDirectionComesOffThePositionAndTheRateNotTheAmountsSign) {
    // mexc does not document the sign of `funding`, so direction comes from position type and rate
    const auto table = catalogue();
    static thread_local FundingRecordBuffer storage;
    const auto body = fixture("rest_funding_records.json");
    const auto rows = parse_funding_records(body, storage);
    ASSERT_TRUE(rows.has_value());

    std::vector<LedgerEntry> booked;
    for (const auto& wire : *rows) {
        if (const auto charge = map_funding_record(wire, table, UnixNanos{7}); charge.has_value()) {
            booked.push_back(*charge);
        }
    }
    // SUSHI_USDT is not loaded, so it is dropped
    ASSERT_EQ(booked.size(), 3U);

    // a long at a negative rate receives
    EXPECT_EQ(booked[0].kind, LedgerKind::Funding);
    EXPECT_EQ(booked[0].amount.raw, 83'779'800);
    // a long at a positive rate pays; the stated amount is positive on both rows
    EXPECT_EQ(booked[1].amount.raw, -12'567'000);
    // a short at a positive rate receives
    EXPECT_EQ(booked[2].amount.raw, 10'279'600);

    EXPECT_EQ(booked[0].settled_at.ns, 1'606'435'200'000ULL * 1'000'000ULL);
    EXPECT_EQ(booked[0].reference.view(), "328033");
    EXPECT_EQ(booked[1].reference.view(), "328034");
    EXPECT_EQ(booked[0].ts_init.ns, 7U);
}

TEST(MexcRest, TheFundingReadIsAccountWideAndCarriesTheCursorItWasGiven) {
    // `symbol` is optional on this endpoint
    net::Signer signer;
    signer.set_api_key("mexc-test-key");
    ASSERT_TRUE(signer.secret().assign("mexc-test-secret").has_value());
    SignedRequestBuilder builder{signer};

    constexpr UnixNanos now{1'700'000'000'000'000'000ULL};
    const auto since = UnixNanos{1'606'435'200'000ULL * 1'000'000ULL};
    const auto request = builder.funding_records(MarketType::Perp, since, now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_EQ(request->target,
              "/api/v1/private/position/funding_records"
              "?page_num=1&page_size=100&start_time=1606435200000");

    // a zero cursor sends no window; the venue's default page applies
    const auto from_zero = builder.funding_records(MarketType::Perp, UnixNanos{0}, now);
    ASSERT_TRUE(from_zero.has_value());
    EXPECT_EQ(from_zero->target, "/api/v1/private/position/funding_records?page_num=1&page_size=100");

    // spot has no funding ledger
    EXPECT_EQ(builder.funding_records(MarketType::Spot, since, now).error(), BuildError::SpotFundingUnsupported);
}

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

TEST(MexcMap, LocalSymbolIsTheCanonicalPairOnBothCatalogues) {
    // `BTCUSDT` has no separator, so the catalogue fields state the pair
    const auto perps = catalogue();
    const auto* perp = perps.find("BTC_USDT");
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(base_of(*perp).exchange_symbol.view(), "BTC_USDT");
    EXPECT_EQ(base_of(*perp).local_symbol.view(), "BTC/USDT");

    const auto spots = spot_catalogue();
    const auto* spot = spots.find("BTCUSDT");
    ASSERT_NE(spot, nullptr);
    EXPECT_EQ(base_of(*spot).exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(base_of(*spot).local_symbol.view(), "BTC/USDT");

    // a row naming no pair is refused
    WireSpotSymbol unnamed{};
    unnamed.symbol = "BTCUSDT";
    unnamed.status = "1";
    unnamed.spot_trading_allowed = true;
    unnamed.base_size_step = "0.000001";
    unnamed.quote_precision = 2;
    EXPECT_FALSE(map_spot_instrument(unnamed).has_value());
    unnamed.base_asset = "BTC";
    unnamed.quote_asset = "USDT";
    EXPECT_TRUE(map_spot_instrument(unnamed).has_value());
}

TEST(MexcSpotStream, TheEnvelopeNamesTheFrameByFieldNumber) {
    // dispatch by payload field number; unknown numbers are skipped
    const auto quote = fixture("ws_spot_book_ticker.pb");
    const auto event = parse_stream_event(quote);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, StreamKind::Quote);
    EXPECT_EQ(event->symbol, "BTCUSDT");
    EXPECT_GT(event->ts_ms, 1'000'000'000'000ULL);
    EXPECT_FALSE(event->payload.empty());

    // json on the spot socket is not a market data message
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
    // tradeType 1 is a taker buy
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
    // market data truncates sizes to the 1e-6 order step from the 1e-8 quote precision
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

TEST(MexcMap, SpotTickerTableYieldsBothVolumesForKnownPairs) {
    const auto table = spot_catalogue();
    const auto body = fixture("rest_spot_ticker_24h.json");
    static thread_local SpotTickerBuffer storage;
    const auto rows = parse_spot_tickers(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);

    std::map<std::string, TickerStats> seen;
    for (const auto& row : *rows) {
        if (const auto stats = map_spot_ticker_stats(row, table, k_ingest); stats.has_value()) {
            seen.emplace(std::string{row.symbol}, *stats);
        }
    }
    EXPECT_FALSE(seen.contains("METALUSDT")) << "not in the catalogue";
    ASSERT_TRUE(seen.contains("BTCUSDT"));
    const auto& btc = seen.at("BTCUSDT");
    EXPECT_EQ(btc.volume_24h_base.raw, 8'277'248'852'990LL);
    EXPECT_EQ(btc.volume_24h_quote.raw, 696'827'729'480'000LL);
    EXPECT_EQ(btc.ts_event.ns, 1'790'329'980'160ULL * 1'000'000ULL);
}

} // namespace
} // namespace raikiri::venues::mexc
