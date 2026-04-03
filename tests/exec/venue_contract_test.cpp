#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/venue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

namespace raikiri::exec {
namespace {

// Captures everything an adapter pushes; stands in for the gateway core.
class CaptureSink final : public GatewaySink {
public:
    void on_bbo(const Bbo& bbo) noexcept override { bbos.push_back(bbo); }

    void on_trade(const TradeTick& trade) noexcept override { trades.push_back(trade); }

    void on_funding(const FundingUpdate& funding) noexcept override { fundings.push_back(funding); }

    void on_order_event(const OrderEvent& event) noexcept override {
        order_events.push_back(event);
    }

    void on_connectivity(const ConnEvent& event) noexcept override { conn_events.push_back(event); }

    std::vector<Bbo> bbos;
    std::vector<TradeTick> trades;
    std::vector<FundingUpdate> fundings;
    std::vector<OrderEvent> order_events;
    std::vector<ConnEvent> conn_events;
};

// Minimal adapter: implements only what a barebones venue would, leans on
// every default the contracts provide. What the defaults do IS the test.
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

    // Supports ONLY BBO; everything else inherits the declining default.
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
        // Contract: outcomes only via the sink — here, instant synthetic ack.
        OrderEvent ev{};
        ev.type = OrderEventType::Accepted;
        ev.instrument = cmd.instrument;
        ev.client_order_id = cmd.client_order_id;
        sink_->on_order_event(ev);
    }

    void cancel(const OrderCmd&) noexcept override { ++cancels; }

    std::expected<std::vector<OrderStatusReport>, VenueError>
    open_order_reports(MarketType) override {
        return std::vector<OrderStatusReport>{};
    }

    std::expected<OrderStatusReport, VenueError> query_order(const ExchangeClientOrderId& id,
                                                             MarketType) override {
        OrderStatusReport report{};
        report.exchange_client_order_id = id;
        report.status = OrderStatus::Accepted;
        return report;
    }

    std::expected<std::vector<FillReport>, VenueError> fill_reports(MarketType,
                                                                    UnixNanos) override {
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
    const auto adapter =
        make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock);
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

    // Bybit has no registered factory in this test binary.
    const auto unregistered = make_venue({.venue = VenueId::Bybit}, sink, clock);
    ASSERT_FALSE(unregistered.has_value());
    EXPECT_EQ(unregistered.error(), MakeError::UnknownVenue);

    // Sim's caps row is perp-only; asking for spot must refuse loudly.
    const auto spot_on_sim =
        make_venue({.venue = VenueId::Sim, .market = MarketType::Spot}, sink, clock);
    ASSERT_FALSE(spot_on_sim.has_value());
    EXPECT_EQ(spot_on_sim.error(), MakeError::UnsupportedMarket);
}

TEST_F(VenueContract, TheCapsRowDecidesEveryMarket) {
    // The config carries ONE market, so "both" and "neither" are no longer
    // sayable and the two refusals that existed to catch them are gone. What
    // is left is the only question worth asking: does this row serve what was
    // asked for. Sim serves perp alone, so the other three must refuse.
    for (const auto market : {MarketType::Spot, MarketType::Future, MarketType::Option}) {
        const auto refused = make_venue({.venue = VenueId::Sim, .market = market}, sink, clock);
        ASSERT_FALSE(refused.has_value()) << to_string(market);
        EXPECT_EQ(refused.error(), MakeError::UnsupportedMarket) << to_string(market);
    }
    EXPECT_TRUE(
        make_venue({.venue = VenueId::Sim, .market = MarketType::Perp}, sink, clock).has_value());
}

TEST_F(VenueContract, AVenueThatSignsWithThreeCredentialsRefusesOnTwo) {
    // okx, kucoin, bitget and coinbase sign with a passphrase as well as a key
    // and a secret. Two of them start cleanly on two credentials and then
    // reject every command locally, so the refusal has to happen here — a
    // gateway that accepts commands it cannot sign looks like a venue outage.
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        const auto& caps = caps_of(id);
        if (!caps.needs_passphrase) {
            continue;
        }
        // A market the row actually serves — coinbase lists no perpetuals, and
        // an UnsupportedMarket refusal would pass this loop for the wrong
        // reason.
        const auto refused =
            make_venue({.venue = id,
                        .market = caps.markets.perp ? MarketType::Perp : MarketType::Spot,
                        .api_key_env = "env:KEY",
                        .api_secret_env = "env:SECRET"},
                       sink,
                       clock);
        ASSERT_FALSE(refused.has_value()) << to_string(id);
        // UnsupportedMarket would mean the row does not serve perp at all,
        // which is a different complaint and must not be mistaken for this one.
        EXPECT_EQ(refused.error(), MakeError::PassphraseRequired) << to_string(id);
    }
}

TEST_F(VenueContract, MarketDataAloneNeedsNoPassphrase) {
    // The whole repository is meant to clone and stream with no credentials at
    // all. A caller that configured none is asking for market data and must not
    // be refused for a credential it never claimed to have.
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        const auto& caps = caps_of(id);
        if (!caps.needs_passphrase) {
            continue;
        }
        const auto built = make_venue(
            {.venue = id, .market = caps.markets.perp ? MarketType::Perp : MarketType::Spot},
            sink,
            clock);
        if (!built.has_value()) {
            // No factory registered in this binary is fine; a passphrase
            // complaint is not.
            EXPECT_NE(built.error(), MakeError::PassphraseRequired) << to_string(id);
        }
    }
}

TEST_F(VenueContract, EveryCapsRowServesWhatItClaimsAndNothingElse) {
    // A row that claims a market its adapter cannot serve is accepted at load
    // and then fails one call at a time — the failure mode this table exists
    // to prevent. Deribit is the only row claiming dated futures or options;
    // if a second one appears, it appears here first.
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
        // Nothing may serve a market with no market at all behind it, and a
        // retired slot must serve none.
        EXPECT_EQ(caps.markets.any(), !is_retired(id)) << to_string(id);
    }
    EXPECT_EQ(with_options, (std::vector<std::string_view>{"deribit"}));
    EXPECT_EQ(with_futures, (std::vector<std::string_view>{"deribit"}));
}

TEST_F(VenueContract, EveryCapsRowCarriesTheUlid) {
    // The whole three-ID recovery scheme rides on 26-char ids fitting every
    // venue we ship. Gate is the tightest fit — this is the tripwire that
    // fires if the docs pass lowers any limit.
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        if (is_retired(static_cast<VenueId>(v))) {
            continue;
        }
        const auto& caps = caps_of(static_cast<VenueId>(v));
        EXPECT_GE(caps.max_client_order_id_len, k_min_client_order_id_len)
            << to_string(static_cast<VenueId>(v));
    }
}

TEST_F(VenueContract, PullCapabilityMatchesWhatTheAdapterWillDo) {
    // The caps row is what the reconciler consults before calling. If a venue
    // claims a pull it has not implemented, the reconciler asks and gets a
    // refusal it did not plan for; if it hides one it has, reconciliation
    // silently never runs. Either way the row and the adapter must agree, so
    // this list grows only when an adapter actually gains the pulls.
    std::vector<std::string_view> claiming;
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        if (caps_of(id).position_pull.any()) {
            claiming.push_back(to_string(id));
        }
        // The two are independent. HTX is the case that proves it: it reports
        // positions, but its trade history is windowed by contract while the
        // pull names only a market, so fills genuinely have no answer. What
        // must hold is the other direction — a venue that cannot report
        // positions has nothing to reconcile against. Asked PER MARKET, and of
        // the derivative markets alone: a spot fill is reconciled against a
        // balance, and no venue here reports a spot position at all.
        for (const auto market : {MarketType::Perp, MarketType::Future, MarketType::Option}) {
            if (caps_of(id).fill_pull.has(market)) {
                EXPECT_TRUE(caps_of(id).position_pull.has(market))
                    << to_string(id) << ": fills without positions leaves nothing to compare";
            }
        }
    }
    // Sorted: this asserts WHICH venues claim the pull, not where they happen
    // to sit in the VenueId enum.
    std::ranges::sort(claiming);
    EXPECT_EQ(claiming,
              (std::vector<std::string_view>{"binance",
                                             "bingx",
                                             "bitfinex",
                                             "bitget",
                                             "bybit",
                                             "deribit",
                                             "gate",
                                             "htx",
                                             "hyperliquid",
                                             "kraken",
                                             "kucoin",
                                             "mexc",
                                             "okx",
                                             "sim",
                                             "weex",
                                             "xt"}));
}

TEST_F(VenueContract, NoRowClaimsAPositionPullOnSpot) {
    // The bug this shape exists to prevent, kept as a tripwire: a spot account
    // holds coins and has no position to report, every adapter here refuses one
    // — and while the claim was a single bit per venue, seven spot gateways
    // asked anyway, once every thirty seconds, for as long as they ran. Each
    // ask is a SIGNED call out of the same per-IP budget the market-data half
    // spends, and each failure line hides one that means something.
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        EXPECT_FALSE(caps_of(id).position_pull.has(MarketType::Spot))
            << to_string(id) << ": a spot account holds balances, not positions";
    }
}

TEST_F(VenueContract, NoPullReachesPastTheMarketsItsAdapterServes) {
    // A pull claimed on a market the adapter does not serve cannot be reached
    // by a gateway — make_venue refuses the config first — so the claim would
    // never be tested by anything but a reader believing it.
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto id = static_cast<VenueId>(v);
        const auto& caps = caps_of(id);
        for (const auto market :
             {MarketType::Spot, MarketType::Perp, MarketType::Future, MarketType::Option}) {
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
    // Naming them: silencing the pulls that cannot answer must not silence the
    // ones that can. Spot open orders REST on every one of these venues, and a
    // row that dropped them would age the open-order table out on a live book —
    // the failure the order pull's own comment calls the worse one.
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
    // ...and the two that genuinely have no account-wide spot form say so.
    EXPECT_FALSE(caps_of(VenueId::Mexc).order_pull.has(MarketType::Spot));
    EXPECT_FALSE(caps_of(VenueId::Kucoin).fill_pull.has(MarketType::Spot));
}

TEST_F(VenueContract, EveryPerpPullTheAdaptersHadIsStillClaimed) {
    // The other half of the fix, and the one worth a test: making the claim
    // per-market must not have quietly dropped a market that works. A perp
    // position pull read as absent is a position the reconciler believes is
    // flat, which is worse than the failures this change removed.
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
    for (const auto id : {VenueId::Binance,
                          VenueId::Okx,
                          VenueId::Gate,
                          VenueId::Kucoin,
                          VenueId::Bingx,
                          VenueId::Xt}) {
        EXPECT_TRUE(caps_of(id).balance_pull.has(MarketType::Perp)) << to_string(id);
    }
    // The two whose wallet is ONE account behind both markets, so the spot arm
    // answers off the same read rather than refusing.
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
                          VenueId::Mexc}) {
        EXPECT_TRUE(caps_of(id).funding_charge.has(MarketType::Perp)) << to_string(id);
    }
    // ...and the two whose ledger cannot answer the question this pull asks:
    // KuCoin's funding history is per-SYMBOL where the pull names a market,
    // and a Bitfinex ledger row names its wallet currency and no contract at
    // all. False here means "contributes no funding PnL", which is the honest
    // state rather than a wrong one.
    EXPECT_FALSE(caps_of(VenueId::Kucoin).funding_charge.has(MarketType::Perp));
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

    // Default amend: deliberate no-op (callers gate on caps.amend_order and
    // fall back to cancel+replace). Must not emit events or crash.
    OrderCmd amend{};
    amend.kind = CmdKind::Amend;
    adapter.execution->amend(amend);
    EXPECT_TRUE(sink.order_events.empty());

    // Targeted single-order recovery round-trips the client id.
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
    // Spot and perpetual share one index space in the table, so an id built by
    // hand with the wrong market half used to resolve to a real instrument that
    // simply was not the one asked for — and every tick and lot derived from it
    // would have been another contract's.
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
    // Same index, another venue: also nothing.
    EXPECT_EQ(table.find(InstrumentId::make(VenueId::Okx, MarketType::Perp, id->symbol())),
              nullptr);
}

TEST_F(VenueContract, OneTableHoldsBothMarketsOfASharedSymbol) {
    // Most venues spell a spot pair and its perpetual identically. Keying the
    // index on the symbol alone made add() refuse the second as a duplicate, so
    // a gateway with both markets enabled loaded whichever catalogue ran first
    // and silently listed none of the other.
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

    // Named with its market, each resolves to its own row.
    EXPECT_EQ(table.find("BTC_USDT", MarketType::Spot), table.find(*spot));
    EXPECT_EQ(table.find("BTC_USDT", MarketType::Perp), table.find(*perp));
    EXPECT_NE(table.find(*spot), table.find(*perp));

    // Named without one, the answer is a refusal — never a coin flip between
    // two instruments whose lot and contract sizes differ.
    EXPECT_EQ(table.find("BTC_USDT"), nullptr);

    // The same symbol in the same market is still a duplicate.
    EXPECT_FALSE(table.add(SpotInstrument{row(MarketType::Spot)}).has_value());
}

TEST_F(VenueContract, ASingleMarketTableStillAnswersOnTheSymbolAlone) {
    // Every gateway today hosts one market, and that path must stay exactly as
    // it was: the bare lookup is the hot path for every inbound frame.
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
