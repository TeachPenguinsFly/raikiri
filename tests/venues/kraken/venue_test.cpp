#include <raikiri/venues/kraken/kraken_venue.hpp>

#include <gtest/gtest.h>

namespace raikiri::venues::kraken {
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

    // Reconciliation pulls answer empty rather than failing: there is nothing
    // of ours here, which is different from being unable to ask.
    EXPECT_TRUE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
}

} // namespace
} // namespace raikiri::venues::kraken
