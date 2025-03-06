#pragma once

// Pre-trade risk gate. Deliberately a pure function of (cmd, open_count):
// no venue lookups, no clocks, no store reference — the caller supplies the
// one piece of mutable state (how many orders are open) so the gate is
// trivially testable and behaves identically under journal replay. A refusal
// becomes a Denied event; nothing refused here ever reaches the wire.
//
// These are structural checks only (an ill-formed command is a bug upstream,
// not a market condition). Notional/position limits need instrument and
// account state and belong to a later, stateful layer.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

namespace raikiri::oms {

enum class RiskReject : std::uint8_t {
    TooManyOpen,
    BadQty,
    QtyTooLarge,
    BadPrice,
    BadInstrument,
    BadFlags,
};

struct RiskConfig {
    std::size_t max_open_orders{64};
    QtyLots max_qty_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};
};

class RiskGate {
public:
    RiskGate() noexcept = default;

    explicit RiskGate(RiskConfig config) noexcept : config_{config} {}

    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }

    // Checks run most-fundamental first: a command that fails several rules
    // reports the one that makes the others meaningless.
    [[nodiscard]] std::expected<void, RiskReject> check(const OrderCmd& cmd,
                                                        std::size_t open_count) const noexcept {
        if (!cmd.instrument.valid()) {
            return std::unexpected{RiskReject::BadInstrument};
        }
        if (cmd.qty.is_undef() || cmd.qty.raw <= 0) {
            return std::unexpected{RiskReject::BadQty};
        }
        if (cmd.qty > config_.max_qty_lots) {
            return std::unexpected{RiskReject::QtyTooLarge};
        }
        if (cmd.order_type == OrderType::Limit && (cmd.price.is_undef() || cmd.price.raw <= 0)) {
            return std::unexpected{RiskReject::BadPrice};
        }
        if (cmd.order_type == OrderType::Market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
            // Post-only is a maker constraint; a market order is a taker by
            // definition. Venues reject this combination — we refuse it first.
            return std::unexpected{RiskReject::BadFlags};
        }
        if (open_count >= config_.max_open_orders) {
            return std::unexpected{RiskReject::TooManyOpen};
        }
        return {};
    }

private:
    RiskConfig config_{};
};

} // namespace raikiri::oms
