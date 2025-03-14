#pragma once

// DemoMaker: the strategy `make demo` runs. Passive two-sided quoting one
// tick inside the touch on every demo instrument, requoted on a timer via
// cancel+replace. Not alpha — order flow: it exists to exercise the whole
// path (risk gate -> OMS -> shm -> matching engine -> fills -> OMS) and to
// feed the latency histograms something honest.

#include <raikiri/strategy/strategy.hpp>

#include <array>
#include <cstdint>

namespace raikiri::strategies {

class DemoMaker final : public strategy::Strategy {
public:
    static constexpr std::uint32_t k_requote_timer = 1;
    static constexpr std::uint64_t k_requote_ns = 250'000'000; // 250 ms
    static constexpr std::size_t k_instruments = 2;

    void on_start(strategy::Context& ctx) override {
        ctx.set_timer(k_requote_timer, k_requote_ns);
    }

    void on_timer(strategy::Context& ctx, std::uint32_t timer_id) override {
        if (timer_id != k_requote_timer) {
            return;
        }
        for (std::size_t i = 0; i < k_instruments; ++i) {
            requote(ctx, i);
        }
        ctx.set_timer(k_requote_timer, k_requote_ns);
    }

    void on_order_event(strategy::Context&, const OrderEvent& event) override {
        if (event.type == OrderEventType::Filled) {
            ++fills_;
        }
        if (event.type == OrderEventType::Filled && event.leaves_qty.raw == 0) {
            clear_if_tracked(event.client_order_id);
        }
        if (event.type == OrderEventType::Canceled || event.type == OrderEventType::Rejected ||
            event.type == OrderEventType::Expired || event.type == OrderEventType::Denied) {
            clear_if_tracked(event.client_order_id);
        }
    }

    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }

private:
    void requote(strategy::Context& ctx, std::size_t idx) {
        const auto instrument =
            InstrumentId::make(VenueId::Sim, MarketType::Perp,
                               SymbolId{static_cast<std::uint32_t>(idx)});
        // Pull the previous pair first; the venue confirms asynchronously.
        auto& pair = quotes_[idx];
        if (pair.bid.valid()) {
            (void)ctx.cancel(pair.bid);
            pair.bid = {};
        }
        if (pair.ask.valid()) {
            (void)ctx.cancel(pair.ask);
            pair.ask = {};
        }

        const auto bbo = ctx.bbo(instrument);
        if (!bbo.has_value() || bbo->bid.is_undef() || bbo->ask.is_undef()) {
            return; // market not formed yet
        }
        const auto qty = QtyLots::from_raw(1);
        strategy::NewOrder bid{};
        bid.instrument = instrument;
        bid.side = Side::Buy;
        bid.price = bbo->bid; // join the best bid
        bid.qty = qty;
        if (const auto id = ctx.submit(bid); id.has_value()) {
            pair.bid = *id;
        }
        strategy::NewOrder ask{};
        ask.instrument = instrument;
        ask.side = Side::Sell;
        ask.price = bbo->ask;
        ask.qty = qty;
        if (const auto id = ctx.submit(ask); id.has_value()) {
            pair.ask = *id;
        }
    }

    void clear_if_tracked(ClientOrderId id) noexcept {
        for (auto& pair : quotes_) {
            if (pair.bid == id) {
                pair.bid = {};
            }
            if (pair.ask == id) {
                pair.ask = {};
            }
        }
    }

    struct QuotePair {
        ClientOrderId bid{};
        ClientOrderId ask{};
    };

    std::array<QuotePair, k_instruments> quotes_{};
    std::uint64_t fills_{0};
};

} // namespace raikiri::strategies
