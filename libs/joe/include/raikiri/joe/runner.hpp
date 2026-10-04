#pragma once

// joe as a strategy: children leave through the kernel's context, so each meets the risk gate

#include <raikiri/joe/desk.hpp>
#include <raikiri/strategy/strategy.hpp>

namespace raikiri::joe {

class ContextSink final : public OrderSink {
public:
    void attach(strategy::Context* ctx) noexcept { ctx_ = ctx; }

    [[nodiscard]] bool running() const noexcept override { return ctx_ != nullptr; }

    [[nodiscard]] std::expected<std::uint64_t, SinkError> submit(const ChildOrder& order) noexcept override;
    void cancel(std::uint64_t child_id) noexcept override;

private:
    strategy::Context* ctx_{nullptr};
};

class Runner final : public strategy::Strategy {
public:
    static constexpr std::uint32_t k_tick_timer = 1;
    static constexpr std::uint64_t k_tick_ns = 250'000'000;

    Runner(Desk& desk, ContextSink& sink) : desk_{&desk}, sink_{&sink} {}

    void on_start(strategy::Context& ctx) override;
    void on_stop(strategy::Context& ctx) override;
    void on_timer(strategy::Context& ctx, std::uint32_t timer_id) override;
    void on_order_event(strategy::Context& ctx, const OrderEvent& event) override;
    void on_trade(strategy::Context& ctx, const TradeTick& trade) override;

private:
    Desk* desk_;
    ContextSink* sink_;
};

} // namespace raikiri::joe
