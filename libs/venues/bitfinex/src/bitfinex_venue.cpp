#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>

#include <memory>
#include <vector>

namespace raikiri::venues::bitfinex {

namespace {

exec::VenueError unimplemented() noexcept {
    return exec::VenueError::make(
        exec::ErrorClass::Terminal, 0, "bitfinex adapter not implemented");
}

class BitfinexData final : public exec::DataClient {
public:
    std::expected<void, exec::VenueError> start() override {
        return std::unexpected{unimplemented()};
    }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }
};

class BitfinexExec final : public exec::ExecutionClient {
public:
    explicit BitfinexExec(exec::GatewaySink& sink, const Clock& clock)
        : sink_{&sink}, clock_{&clock} {}

    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    void submit(const OrderCmd& cmd) noexcept override { emit(cmd, OrderEventType::Rejected); }

    void cancel(const OrderCmd& cmd) noexcept override {
        emit(cmd, OrderEventType::CancelRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        return std::unexpected{unimplemented()};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId&, MarketType) override {
        return std::unexpected{unimplemented()};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos) override {
        return std::unexpected{unimplemented()};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{unimplemented()};
    }

private:
    void emit(const OrderCmd& cmd, OrderEventType type) noexcept {
        OrderEvent event{};
        event.type = type;
        event.side = cmd.side;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        event.ts_event = clock_->wall();
        event.ts_init = event.ts_event;
        sink_->on_order_event(event);
    }

    exec::GatewaySink* sink_;
    const Clock* clock_;
};

class BitfinexInstruments final : public exec::InstrumentSource {
public:
    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType) override {
        return std::unexpected{unimplemented()};
    }
};

exec::VenueAdapter make(const exec::VenueConfig&, exec::GatewaySink& sink, const Clock& clock) {
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bitfinex);
    adapter.data = std::make_unique<BitfinexData>();
    adapter.execution = std::make_unique<BitfinexExec>(sink, clock);
    adapter.instruments = std::make_unique<BitfinexInstruments>();
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitfinex, &make);
}

} // namespace raikiri::venues::bitfinex
