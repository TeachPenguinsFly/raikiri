#include <raikiri/domain/demo_instruments.hpp>
#include <raikiri/simcore/engine.hpp>
#include <raikiri/simcore/mdgen.hpp>
#include <raikiri/venues/sim/sim_venue.hpp>

#include <memory>
#include <vector>

namespace raikiri::venues::sim {

namespace {

// Shared venue state: one Engine + MdGen behind both client facades. The
// gateway thread is the only caller (single-threaded by architecture), so no
// synchronization exists here on purpose.
struct SimState {
    explicit SimState(exec::GatewaySink& sink) : sink_ref{sink}, mdgen{k_seed} {}

    static constexpr std::uint64_t k_seed = 0x5EED'CAFE'F00DULL;
    static constexpr std::uint64_t k_step_interval_ns = 1'000'000; // 1 ms

    exec::GatewaySink& sink_ref;
    simcore::Engine engine;
    simcore::MdGen mdgen;
    std::vector<InstrumentId> active; // instruments being generated
    UnixNanos last_step{};

    void publish(const simcore::SimResult& result, InstrumentId instrument, UnixNanos now) {
        for (const auto& event : result.events) {
            sink_ref.on_order_event(event);
        }
        for (const auto& trade : result.trades) {
            sink_ref.on_trade(trade);
        }
        if (result.bbo_changed) {
            publish_bbo(instrument, now);
        }
    }

    void publish_bbo(InstrumentId instrument, UnixNanos now) {
        const auto levels = engine.bbo(instrument);
        if (!levels.has_value()) {
            return;
        }
        Bbo bbo{};
        bbo.instrument = instrument;
        bbo.bid = levels->bid;
        bbo.ask = levels->ask;
        bbo.bid_qty = levels->bid_qty;
        bbo.ask_qty = levels->ask_qty;
        bbo.ts_event = now;
        bbo.ts_init = now;
        sink_ref.on_bbo(bbo);
    }
};

class SimData final : public exec::DataClient {
public:
    explicit SimData(std::shared_ptr<SimState> state) : state_{std::move(state)} {}

    std::expected<void, exec::VenueError> start() override {
        state_->sink_ref.on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = 0, .ts = {}});
        started_ = true;
        return {};
    }

    void stop() override { started_ = false; }

    [[nodiscard]] bool connected() const noexcept override { return started_; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (kind != exec::SubKind::Bbo && kind != exec::SubKind::Trades) {
            return DataClient::subscribe(kind, instrument);
        }
        for (const auto active : state_->active) {
            if (active == instrument) {
                return {};
            }
        }
        const auto idx = instrument.symbol().raw;
        if (idx >= std::size(demo::k_demo_mid_ticks)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown sim instrument")};
        }
        state_->mdgen.add_instrument(instrument, PriceTicks::from_raw(demo::k_demo_mid_ticks[idx]));
        state_->active.push_back(instrument);
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_ || now.ns < state_->last_step.ns + SimState::k_step_interval_ns) {
            return;
        }
        state_->last_step = now;
        for (const auto instrument : state_->active) {
            const auto result = state_->mdgen.step(state_->engine, instrument, now);
            state_->publish(result, instrument, now);
            state_->publish_bbo(instrument, now);
        }
    }

private:
    std::shared_ptr<SimState> state_;
    bool started_{false};
};

class SimExec final : public exec::ExecutionClient {
public:
    explicit SimExec(std::shared_ptr<SimState> state, const Clock& clock)
        : state_{std::move(state)}, clock_{&clock} {}

    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }

    void submit(const OrderCmd& cmd) noexcept override { run(cmd); }

    void cancel(const OrderCmd& cmd) noexcept override { run(cmd); }

    void amend(const OrderCmd& cmd) noexcept override { run(cmd); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        std::vector<exec::OrderStatusReport> reports;
        for (const auto& meta : state_->engine.open_client_orders()) {
            exec::OrderStatusReport report{};
            report.exchange_client_order_id = meta.exchange_client_order_id;
            report.exchange_order_id = meta.exchange_order_id;
            report.instrument = meta.instrument;
            report.side = meta.side;
            report.status =
                meta.leaves == meta.qty ? OrderStatus::Accepted : OrderStatus::PartiallyFilled;
            report.price = meta.price;
            report.qty = meta.qty;
            report.leaves = meta.leaves;
            report.ts_venue = clock_->wall();
            reports.push_back(report);
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        auto reports = open_order_reports(market);
        for (const auto& report : *reports) {
            if (report.exchange_client_order_id == id) {
                return report;
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open on sim")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos) override {
        return std::unexpected{exec::not_implemented("sim reconciliation pulls")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{exec::not_implemented("sim reconciliation pulls")};
    }

private:
    void run(const OrderCmd& cmd) noexcept {
        const auto now = clock_->wall();
        const auto result = state_->engine.handle(cmd, now);
        state_->publish(result, cmd.instrument, now);
    }

    std::shared_ptr<SimState> state_;
    const Clock* clock_;
};

class SimInstruments final : public exec::InstrumentSource {
public:
    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::vector<Instrument>{};
        }
        return demo::sim_instruments();
    }
};

exec::VenueAdapter make_sim(const exec::VenueConfig&, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<SimState>(sink);
    exec::VenueAdapter adapter;
    adapter.caps = exec::caps_of(VenueId::Sim);
    adapter.data = std::make_unique<SimData>(state);
    adapter.execution = std::make_unique<SimExec>(state, clock);
    adapter.instruments = std::make_unique<SimInstruments>();
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Sim, &make_sim);
}

} // namespace raikiri::venues::sim
