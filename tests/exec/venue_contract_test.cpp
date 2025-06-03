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

    void on_funding(const FundingUpdate& funding) noexcept override {
        fundings.push_back(funding);
    }

    void on_order_event(const OrderEvent& event) noexcept override {
        order_events.push_back(event);
    }

    void on_connectivity(const ConnEvent& event) noexcept override {
        conn_events.push_back(event);
    }

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
    const auto adapter = make_venue({.venue = VenueId::Sim, .enable_perp = true}, sink, clock);
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
        make_venue({.venue = VenueId::Sim, .enable_spot = true, .enable_perp = true}, sink, clock);
    ASSERT_FALSE(spot_on_sim.has_value());
    EXPECT_EQ(spot_on_sim.error(), MakeError::UnsupportedMarket);
}

TEST_F(VenueContract, EveryCapsRowCarriesTheUlid) {
    // The whole three-ID recovery scheme rides on 26-char ids fitting every
    // venue we ship. Gate is the tightest fit — this is the tripwire that
    // fires if the docs pass lowers any limit.
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
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
        if (caps_of(id).position_pull) {
            claiming.push_back(to_string(id));
        }
        // The two are independent. HTX is the case that proves it: it reports
        // positions, but its trade history is windowed by contract while the
        // pull names only a market, so fills genuinely have no answer. What
        // must hold is the other direction — a venue that cannot report
        // positions has nothing to reconcile against.
        if (caps_of(id).fill_pull) {
            EXPECT_TRUE(caps_of(id).position_pull)
                << to_string(id) << ": fills without positions leaves nothing to compare";
        }
    }
    // Sorted: this asserts WHICH venues claim the pull, not where they happen
    // to sit in the VenueId enum.
    std::ranges::sort(claiming);
    EXPECT_EQ(claiming,
              (std::vector<std::string_view>{"binance", "bitget", "bybit", "gate", "htx",
                                             "kucoin", "mexc", "okx"}));
}

TEST_F(VenueContract, DefaultSubscribeDeclinesUnsupportedKinds) {
    auto adapter = *make_venue({.venue = VenueId::Sim, .enable_perp = true}, sink, clock);
    const auto instrument = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1});

    ASSERT_TRUE(adapter.data->subscribe(SubKind::Bbo, instrument).has_value());
    const auto klines = adapter.data->subscribe(SubKind::Kline, instrument);
    ASSERT_FALSE(klines.has_value());
    EXPECT_EQ(klines.error().cls, ErrorClass::Terminal); // decline, not crash
}

TEST_F(VenueContract, OrderOutcomesArriveOnlyViaSink) {
    auto adapter = *make_venue({.venue = VenueId::Sim, .enable_perp = true}, sink, clock);

    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{7});
    cmd.client_order_id = ClientOrderId::make(1, 99);
    adapter.execution->submit(cmd);

    ASSERT_EQ(sink.order_events.size(), 1U);
    EXPECT_EQ(sink.order_events[0].type, OrderEventType::Accepted);
    EXPECT_EQ(sink.order_events[0].client_order_id.raw, cmd.client_order_id.raw);
}

TEST_F(VenueContract, DefaultAmendIsInertAndQueryOrderRecovers) {
    auto adapter = *make_venue({.venue = VenueId::Sim, .enable_perp = true}, sink, clock);

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
    auto adapter = *make_venue({.venue = VenueId::Sim, .enable_perp = true}, sink, clock);
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

} // namespace
} // namespace raikiri::exec
