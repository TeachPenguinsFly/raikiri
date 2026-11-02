#pragma once

// what a cancel or amend of a live order should do; a repeat is a no-op, not an error

#include <raikiri/common/time.hpp>
#include <raikiri/oms/order.hpp>
#include <raikiri/oms/order_store.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace raikiri::oms {

enum class CommandEffect : std::uint8_t { Send, NoOp };

// an unanswered cancel may have been lost, so past this it goes again; below joe's own retry, or that one is swallowed
inline constexpr std::uint64_t k_cancel_resend_ns = 3ULL * 1'000'000'000ULL;

[[nodiscard]] constexpr CommandEffect cancel_effect(const Order& order, UnixNanos now) noexcept {
    const bool asked = order.status() == OrderStatus::PendingCancel;
    return asked && now.ns - order.ts_cancel_sent().ns < k_cancel_resend_ns ? CommandEffect::NoOp : CommandEffect::Send;
}

[[nodiscard]] constexpr CommandEffect amend_effect(const Order& order, PriceTicks price) noexcept {
    if (order.status() == OrderStatus::PendingCancel) {
        return CommandEffect::NoOp;
    }
    const auto aimed_at = order.status() == OrderStatus::PendingUpdate ? order.pending_price() : order.price();
    return aimed_at == price ? CommandEffect::NoOp : CommandEffect::Send;
}

// how long an order may be missing from a pull before that means anything: the pull can overtake an order still on
// its way to the venue
inline constexpr std::uint64_t k_recon_order_grace_ns = 10ULL * 1'000'000'000ULL;

// open here on this account and absent from a whole pull of its open orders
[[nodiscard]] std::vector<const Order*> orders_gone(const OrderStore& store,
                                                    AccountId account,
                                                    std::span<const ExchangeClientOrderId> at_venue,
                                                    UnixNanos pulled);

} // namespace raikiri::oms
