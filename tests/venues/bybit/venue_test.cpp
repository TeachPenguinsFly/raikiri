#include <raikiri/venues/bybit/bybit_venue.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri::venues::bybit {
namespace {

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo& bbo) noexcept override { bbos.push_back(bbo); }
    void on_trade(const TradeTick& trade) noexcept override { trades.push_back(trade); }
    void on_funding(const FundingUpdate& funding) noexcept override { fundings.push_back(funding); }
    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }
    void on_connectivity(const exec::ConnEvent& event) noexcept override { conn.push_back(event); }

    std::vector<Bbo> bbos;
    std::vector<TradeTick> trades;
    std::vector<FundingUpdate> fundings;
    std::vector<OrderEvent> events;
    std::vector<exec::ConnEvent> conn;
};

class BybitVenue : public ::testing::Test {
protected:
    void SetUp() override { register_adapter(); }

    CaptureSink sink;
    LiveClock clock;
};

TEST_F(BybitVenue, RegistersAndBuildsWithoutCredentials) {
    // Market data needs no keys, so a gateway started without them still
    // runs — read-only by construction rather than by configuration.
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Bybit, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bybit);
    EXPECT_TRUE(adapter->caps.perp);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST_F(BybitVenue, CapsRowCarriesOurOrderIds) {
    // The whole three-id recovery scheme rides on a 26-char ULID fitting the
    // venue's client-order-id field.
    const auto& caps = exec::caps_of(VenueId::Bybit);
    EXPECT_GE(caps.max_client_order_id_len, exec::k_min_client_order_id_len);
    EXPECT_TRUE(caps.cancel_by_client_order_id);
    EXPECT_TRUE(caps.ws_order_entry);
    EXPECT_FALSE(caps.fix_order_entry) << "bybit offers no FIX; its low-latency path is WS";
    EXPECT_FALSE(caps.book_checksum) << "bybit never shipped one; update ids are the guarantee";
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::AuthSubscribe);
}

TEST_F(BybitVenue, SubscribingAnUnknownInstrumentIsRefused) {
    auto adapter_holder = exec::make_venue({.venue = VenueId::Bybit, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter_holder.has_value());
    auto& adapter = *adapter_holder;
    const auto unknown = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7});
    const auto result = adapter.data->subscribe(exec::SubKind::Bbo, unknown);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().cls, exec::ErrorClass::Terminal);
}

TEST_F(BybitVenue, OrderWithoutCredentialsFailsLoudlyAsAnEvent) {
    // A command that cannot even be built must still produce a terminal
    // event: silence would leave the order open in the store forever.
    auto adapter_holder = exec::make_venue({.venue = VenueId::Bybit, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter_holder.has_value());
    auto& adapter = *adapter_holder;

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.side = Side::Buy;
    cmd.qty = QtyLots::from_raw(1);
    adapter.execution->submit(cmd);

    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].client_order_id.raw, cmd.client_order_id.raw);

    // A cancel that cannot be built is a CancelRejected, not a Rejected: the
    // order it targets is still alive.
    cmd.kind = CmdKind::Cancel;
    adapter.execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

TEST_F(BybitVenue, StopIsIdempotent) {
    auto adapter_holder = exec::make_venue({.venue = VenueId::Bybit, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter_holder.has_value());
    auto& adapter = *adapter_holder;
    adapter.data->stop();
    adapter.data->stop();
    adapter.execution->stop();
    adapter.execution->stop();
    SUCCEED() << "teardown calls stop more than once";
}


TEST(BybitCaps, ClaimsThePullsItImplements) {
    // The reconciler reads this row instead of calling and guessing from an
    // empty vector. Bybit answers, so it says so.
    EXPECT_TRUE(exec::caps_of(VenueId::Bybit).position_pull);
    EXPECT_TRUE(exec::caps_of(VenueId::Bybit).fill_pull);
}

} // namespace
} // namespace raikiri::venues::bybit
