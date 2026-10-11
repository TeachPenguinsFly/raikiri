#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/venue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

namespace raikiri::exec {
namespace {

// captures everything an adapter pushes
class CaptureSink final : public GatewaySink {
public:
    void on_bbo(const Bbo& bbo) noexcept override { bbos.push_back(bbo); }

    void on_trade(const TradeTick& trade) noexcept override { trades.push_back(trade); }

    void on_funding(const FundingUpdate& funding) noexcept override { fundings.push_back(funding); }

    void on_order_event(const OrderEvent& event) noexcept override { order_events.push_back(event); }

    void on_connectivity(const ConnEvent& event) noexcept override { conn_events.push_back(event); }

    std::vector<Bbo> bbos;
    std::vector<TradeTick> trades;
    std::vector<FundingUpdate> fundings;
    std::vector<OrderEvent> order_events;
    std::vector<ConnEvent> conn_events;
};

// implements only what a minimal venue would and relies on the defaults
class MockData final : public DataClient {
public:
    explicit MockData(GatewaySink& sink) : sink_{&sink} {}

    std::expected<void, VenueError> start() override {
        sink_->on_connectivity({.state = ConnState::Connected, .stream_id = 1, .ts = {}});
        started = true;
        return {};
    }

    void stop() override { started = false; }

    [[nodiscard]] bool connected() const noexcept override { return started; }

    std::expected<void, VenueError> subscribe(SubKind kind, InstrumentId instrument) override {
        if (kind == SubKind::Bbo) {
            subscribed.push_back(instrument);
            return {};
        }
        return DataClient::subscribe(kind, instrument);
    }

    bool started{false};
    std::vector<InstrumentId> subscribed;

private:
    GatewaySink* sink_;
};

class MockExec final : public ExecutionClient {
public:
    explicit MockExec(GatewaySink& sink) : sink_{&sink} {}

    std::expected<void, VenueError> start() override { return {}; }

    void stop() override { ++stop_calls; } // idempotency probed below

    [[nodiscard]] bool connected() const noexcept override { return true; }

    void submit(const OrderCmd& cmd) noexcept override {
        OrderEvent ev{};
        ev.type = OrderEventType::Accepted;
        ev.instrument = cmd.instrument;
        ev.client_order_id = cmd.client_order_id;
        sink_->on_order_event(ev);
    }

    void cancel(const OrderCmd&) noexcept override { ++cancels; }

    std::expected<std::vector<OrderStatusReport>, VenueError> open_order_reports(MarketType) override {
        return std::vector<OrderStatusReport>{};
    }

    std::expected<OrderStatusReport, VenueError> query_order(const ExchangeClientOrderId& id, MarketType) override {
        OrderStatusReport report{};
        report.exchange_client_order_id = id;
        report.status = OrderStatus::Accepted;
        return report;
    }

    std::expected<std::vector<FillReport>, VenueError> fill_reports(MarketType, UnixNanos) override {
        return std::vector<FillReport>{};
    }

    std::expected<std::vector<PositionReport>, VenueError> position_reports(MarketType) override {
        return std::vector<PositionReport>{};
    }

    int cancels{0};
    int stop_calls{0};

private:
    GatewaySink* sink_;
};

class MockInstruments final : public InstrumentSource {
public:
    std::expected<std::vector<Instrument>, VenueError> fetch_all(MarketType) override {
        return std::vector<Instrument>{};
    }
};

VenueAdapter make_mock(const VenueConfig&, GatewaySink& sink, const Clock&) {
    VenueAdapter adapter;
    adapter.caps = caps_of(VenueId::Sim);
    adapter.data = std::make_unique<MockData>(sink);
    adapter.execution = std::make_unique<MockExec>(sink);
    adapter.instruments = std::make_unique<MockInstruments>();
    return adapter;
}

class VenueContract : public ::testing::Test {
protected:
    void SetUp() override { register_venue(VenueId::Sim, &make_mock); }

    CaptureSink sink;
    LiveClock clock;
};

TEST_F(VenueContract, RegistryBuildsRegisteredVenue) {
    const auto adapter = make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Sim);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST_F(VenueContract, UnknownAndUnsupportedConfigsRefusedAtStartup) {
    const auto unknown = make_venue({.venue = VenueId::Invalid}, sink, clock);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error(), MakeError::UnknownVenue);

    // no bybit factory is registered in this test binary
    const auto unregistered = make_venue({.venue = VenueId::Bybit}, sink, clock);
    ASSERT_FALSE(unregistered.has_value());
    EXPECT_EQ(unregistered.error(), MakeError::UnknownVenue);

    const auto spot_on_sim = make_venue({.venue = VenueId::Sim, .market = MarketType::Spot}, sink, clock);
    ASSERT_FALSE(spot_on_sim.has_value());
    EXPECT_EQ(spot_on_sim.error(), MakeError::UnsupportedMarket);
}

TEST_F(VenueContract, TheCapsRowDecidesEveryMarket) {
    for (const auto market : {MarketType::Spot, MarketType::Future, MarketType::Option}) {
        const auto refused = make_venue({.venue = VenueId::Sim, .market = market}, sink, clock);
        ASSERT_FALSE(refused.has_value()) << to_string(market);
        EXPECT_EQ(refused.error(), MakeError::UnsupportedMarket) << to_string(market);
    }
    EXPECT_TRUE(make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock).has_value());
}

TEST_F(VenueContract, AVenueThatSignsWithThreeCredentialsRefusesOnTwo) {
    // okx, kucoin, bitget and coinbase also sign with a passphrase
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        const auto& caps = caps_of(id);
        if (!caps.needs_passphrase) {
            continue;
        }
        // a market the row serves, so an UnsupportedMarket refusal cannot pass the loop
        const auto refused = make_venue({.venue = id,
                                         .market = caps.markets.perp ? MarketType::Perp : MarketType::Spot,
                                         .api_key_env = "env:KEY",
                                         .api_secret_env = "env:SECRET"},
                                        sink,
                                        clock);
        ASSERT_FALSE(refused.has_value()) << to_string(id);
        EXPECT_EQ(refused.error(), MakeError::PassphraseRequired) << to_string(id);
    }
}

TEST_F(VenueContract, MarketDataAloneNeedsNoPassphrase) {
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        const auto& caps = caps_of(id);
        if (!caps.needs_passphrase) {
            continue;
        }
        const auto built = make_venue(
            {.venue = id, .market = caps.markets.perp ? MarketType::Perp : MarketType::Spot}, sink, clock);
        if (!built.has_value()) {
            EXPECT_NE(built.error(), MakeError::PassphraseRequired) << to_string(id);
        }
    }
}

TEST_F(VenueContract, EveryCapsRowServesWhatItClaimsAndNothingElse) {
    std::vector<std::string_view> with_options;
    std::vector<std::string_view> with_futures;
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        const auto& caps = caps_of(id);
        if (caps.markets.option) {
            with_options.push_back(to_string(id));
        }
        if (caps.markets.future) {
            with_futures.push_back(to_string(id));
        }
        EXPECT_EQ(caps.markets.any(), !is_retired(id)) << to_string(id);
    }
    EXPECT_EQ(with_options, (std::vector<std::string_view>{"binance", "bybit", "okx", "gate", "deribit", "derive"}));
    EXPECT_EQ(with_futures, (std::vector<std::string_view>{"deribit"}));
}

TEST_F(VenueContract, EveryCapsRowCarriesTheUlid) {
    // gate has the tightest client id limit for 26-char ulids
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        if (is_retired(id)) {
            continue;
        }
        const auto& caps = caps_of(id);
        if (caps.max_client_order_id_len == 0) {
            EXPECT_EQ(caps.user_stream, UserStreamModel::None) << to_string(id);
            EXPECT_FALSE(caps.position_pull.any() || caps.order_pull.any() || caps.fill_pull.any()) << to_string(id);
            continue;
        }
        EXPECT_GE(caps.max_client_order_id_len, k_min_client_order_id_len) << to_string(id);
    }
}

TEST_F(VenueContract, ARowThatCannotCarryTheUlidIsRefusedOnlyToATrader) {
    const auto& caps = caps_of(VenueId::Apex);
    ASSERT_LT(caps.max_client_order_id_len, k_min_client_order_id_len);
    const auto trader = make_venue(
        {.venue = VenueId::Apex, .market = MarketType::Perp, .api_key_env = "env:KEY", .api_secret_env = "env:SECRET"},
        sink,
        clock);
    ASSERT_FALSE(trader.has_value());
    EXPECT_EQ(trader.error(), MakeError::ClientOrderIdTooShort);
    const auto reader = make_venue({.venue = VenueId::Apex, .market = MarketType::Perp}, sink, clock);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error(), MakeError::UnknownVenue);
}

TEST_F(VenueContract, PullCapabilityMatchesWhatTheAdapterWillDo) {
    std::vector<std::string_view> claiming;
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        if (caps_of(id).position_pull.any()) {
            claiming.push_back(to_string(id));
        }
        // htx reports positions but cannot pull fills by market alone
        for (const auto market : {MarketType::Perp, MarketType::Future, MarketType::Option}) {
            if (caps_of(id).fill_pull.has(market)) {
                EXPECT_TRUE(caps_of(id).position_pull.has(market))
                    << to_string(id) << ": fills without positions leaves nothing to compare";
            }
        }
    }
    std::ranges::sort(claiming);
    EXPECT_EQ(claiming,
              (std::vector<std::string_view>{"aster",       "binance", "bingx",   "bitfinex", "bitget", "bitunix",
                                             "blofin",      "bybit",   "deribit", "edgex",    "gate",   "htx",
                                             "hyperliquid", "kraken",  "kucoin",  "lighter",  "mexc",   "okx",
                                             "phemex",      "sim",     "toobit",  "weex",     "xt"}));
}

TEST_F(VenueContract, NoRowClaimsAPositionPullOnSpot) {
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        EXPECT_FALSE(caps_of(id).position_pull.has(MarketType::Spot))
            << to_string(id) << ": a spot account holds balances, not positions";
    }
}

TEST_F(VenueContract, NoPullReachesPastTheMarketsItsAdapterServes) {
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        const auto& caps = caps_of(id);
        for (const auto market : {MarketType::Spot, MarketType::Perp, MarketType::Future, MarketType::Option}) {
            if (caps.serves(market)) {
                continue;
            }
            EXPECT_FALSE(caps.position_pull.has(market)) << to_string(id);
            EXPECT_FALSE(caps.order_pull.has(market)) << to_string(id);
            EXPECT_FALSE(caps.fill_pull.has(market)) << to_string(id);
            EXPECT_FALSE(caps.balance_pull.has(market)) << to_string(id);
            EXPECT_FALSE(caps.funding_charge.has(market)) << to_string(id);
        }
    }
}

TEST_F(VenueContract, TheSpotFleetIsLeftWithThePullsThatAnswer) {
    for (const auto id : {VenueId::Bybit,
                          VenueId::Okx,
                          VenueId::Gate,
                          VenueId::Kucoin,
                          VenueId::Bingx,
                          VenueId::Xt,
                          VenueId::Htx,
                          VenueId::Coinbase}) {
        EXPECT_TRUE(caps_of(id).order_pull.has(MarketType::Spot)) << to_string(id);
    }
    EXPECT_FALSE(caps_of(VenueId::Mexc).order_pull.has(MarketType::Spot));
    EXPECT_FALSE(caps_of(VenueId::Kucoin).fill_pull.has(MarketType::Spot));
}

TEST_F(VenueContract, EveryPerpPullTheAdaptersHadIsStillClaimed) {
    for (const auto id : {VenueId::Binance,
                          VenueId::Bybit,
                          VenueId::Okx,
                          VenueId::Gate,
                          VenueId::Kucoin,
                          VenueId::Bingx,
                          VenueId::Xt,
                          VenueId::Mexc,
                          VenueId::Deribit,
                          VenueId::Hyperliquid,
                          VenueId::Kraken,
                          VenueId::Bitget,
                          VenueId::Htx,
                          VenueId::Bitfinex,
                          VenueId::Sim}) {
        EXPECT_TRUE(caps_of(id).position_pull.has(MarketType::Perp)) << to_string(id);
        EXPECT_TRUE(caps_of(id).order_pull.has(MarketType::Perp)) << to_string(id);
    }
    for (const auto id :
         {VenueId::Binance, VenueId::Okx, VenueId::Gate, VenueId::Kucoin, VenueId::Bingx, VenueId::Xt}) {
        EXPECT_TRUE(caps_of(id).balance_pull.has(MarketType::Perp)) << to_string(id);
    }
    // these share one wallet across spot and perp
    EXPECT_TRUE(caps_of(VenueId::Bybit).balance_pull.has(MarketType::Spot));
    EXPECT_TRUE(caps_of(VenueId::Okx).balance_pull.has(MarketType::Spot));
    for (const auto id : {VenueId::Binance,
                          VenueId::Bybit,
                          VenueId::Okx,
                          VenueId::Gate,
                          VenueId::Bingx,
                          VenueId::Xt,
                          VenueId::Bitget,
                          VenueId::Htx,
                          VenueId::Hyperliquid,
                          VenueId::Mexc,
                          VenueId::Kucoin,
                          VenueId::Deribit}) {
        EXPECT_TRUE(caps_of(id).funding_charge.has(MarketType::Perp)) << to_string(id);
    }
    // a bitfinex ledger row names no contract
    EXPECT_FALSE(caps_of(VenueId::Bitfinex).funding_charge.has(MarketType::Perp));
}

TEST_F(VenueContract, DefaultSubscribeDeclinesUnsupportedKinds) {
    auto adapter = *make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock);
    const auto instrument = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1});

    ASSERT_TRUE(adapter.data->subscribe(SubKind::Bbo, instrument).has_value());
    const auto klines = adapter.data->subscribe(SubKind::Kline, instrument);
    ASSERT_FALSE(klines.has_value());
    EXPECT_EQ(klines.error().cls, ErrorClass::Terminal); // decline, not crash
}

TEST_F(VenueContract, OrderOutcomesArriveOnlyViaSink) {
    auto adapter = *make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock);

    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{7});
    cmd.client_order_id = ClientOrderId::make(1, 99);
    adapter.execution->submit(cmd);

    ASSERT_EQ(sink.order_events.size(), 1U);
    EXPECT_EQ(sink.order_events[0].type, OrderEventType::Accepted);
    EXPECT_EQ(sink.order_events[0].client_order_id.raw, cmd.client_order_id.raw);
}

TEST_F(VenueContract, DefaultAmendIsInertAndQueryOrderRecovers) {
    auto adapter = *make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock);

    // the default amend is a no-op; callers gate on caps.amend_order
    OrderCmd amend{};
    amend.kind = CmdKind::Amend;
    adapter.execution->amend(amend);
    EXPECT_TRUE(sink.order_events.empty());

    UlidGen ulids{7};
    const auto id = ulids.next(UnixNanos{1});
    const auto report = adapter.execution->query_order(id, MarketType::Perp);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->exchange_client_order_id.view(), id.view());
}

TEST_F(VenueContract, DefaultBatchCancelLoops) {
    auto adapter = *make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock);
    auto* exec = dynamic_cast<MockExec*>(adapter.execution.get());
    ASSERT_NE(exec, nullptr);

    const std::array<OrderCmd, 3> cmds{};
    adapter.execution->cancel_batch(cmds);
    EXPECT_EQ(exec->cancels, 3);

    adapter.execution->stop();
    adapter.execution->stop(); // idempotent by contract
    EXPECT_EQ(exec->stop_calls, 2);
}

TEST_F(VenueContract, AWrongMarketIdResolvesToNothingRatherThanTheWrongInstrument) {
    InstrumentTable table;
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{0});
    base.local_symbol = *SymbolText::from("BTCUSDT");
    base.exchange_symbol = *SymbolText::from("BTCUSDT");
    base.tick_size = *parse_decimal_e9("0.1");
    base.lot_size = *parse_decimal_e9("0.001");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    const auto id = table.add(PerpInstrument{base, 28'800});
    ASSERT_TRUE(id.has_value());

    EXPECT_NE(table.find(*id), nullptr);
    EXPECT_EQ(table.exchange_symbol(*id), "BTCUSDT");

    for (const auto wrong : {MarketType::Spot, MarketType::Option, MarketType::Future}) {
        const auto other = InstrumentId::make(VenueId::Bybit, wrong, id->symbol());
        EXPECT_EQ(table.find(other), nullptr) << static_cast<int>(wrong);
        EXPECT_TRUE(table.exchange_symbol(other).empty()) << static_cast<int>(wrong);
    }
    EXPECT_EQ(table.find(InstrumentId::make(VenueId::Okx, MarketType::Perp, id->symbol())), nullptr);
}

TEST_F(VenueContract, OneTableHoldsBothMarketsOfASharedSymbol) {
    const auto row = [](MarketType market) {
        InstrumentBase base{};
        base.id = InstrumentId::make(VenueId::Gate, market, SymbolId{0});
        base.local_symbol = *SymbolText::from("BTC_USDT");
        base.exchange_symbol = *SymbolText::from("BTC_USDT");
        base.tick_size = *parse_decimal_e9("0.1");
        base.lot_size = *parse_decimal_e9("0.001");
        base.contract_value = *parse_decimal_e9("1");
        base.kind = ContractKind::Linear;
        return base;
    };
    InstrumentTable table;
    const auto spot = table.add(SpotInstrument{row(MarketType::Spot)});
    const auto perp = table.add(PerpInstrument{row(MarketType::Perp), 28'800});
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(perp.has_value()) << "the second market must not read as a duplicate";
    EXPECT_EQ(table.size(), 2U);

    EXPECT_EQ(table.find("BTC_USDT", MarketType::Spot), table.find(*spot));
    EXPECT_EQ(table.find("BTC_USDT", MarketType::Perp), table.find(*perp));
    EXPECT_NE(table.find(*spot), table.find(*perp));

    EXPECT_EQ(table.find("BTC_USDT"), nullptr);

    EXPECT_FALSE(table.add(SpotInstrument{row(MarketType::Spot)}).has_value());
}

TEST_F(VenueContract, ASingleMarketTableStillAnswersOnTheSymbolAlone) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Gate, MarketType::Perp, SymbolId{0});
    base.local_symbol = *SymbolText::from("BTC_USDT");
    base.exchange_symbol = *SymbolText::from("BTC_USDT");
    base.tick_size = *parse_decimal_e9("0.1");
    base.lot_size = *parse_decimal_e9("0.001");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;

    InstrumentTable table;
    const auto id = table.add(PerpInstrument{base, 28'800});
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(table.find("BTC_USDT"), table.find(*id));
    EXPECT_EQ(table.find("BTC_USDT", MarketType::Perp), table.find(*id));
    EXPECT_EQ(table.find("BTC_USDT", MarketType::Spot), nullptr);
    EXPECT_EQ(table.find("NOSUCH"), nullptr);
}

} // namespace
} // namespace raikiri::exec
