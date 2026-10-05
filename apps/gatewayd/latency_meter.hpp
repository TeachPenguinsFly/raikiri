#pragma once

// per-message latency series for the gateway thread

#include <raikiri/common/time.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/introspect/feed_window.hpp>
#include <raikiri/introspect/latency.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "stats_collector.hpp"

namespace raikiri::gatewayd {

class LatencyMeter {
public:
    static constexpr std::size_t k_metered = 4; // bbo, trades, depth, funding

    LatencyMeter(introspect::LatencyBook& book, const Clock& clock) : clock_{&clock} {
        for (std::size_t i = 0; i < k_metered; ++i) {
            feed_[i] = &book.series(k_channel_names[i], introspect::Stage::Feed);
            internal_[i] = &book.series(k_channel_names[i], introspect::Stage::Internal);
        }
        ack_ = &book.series("orders", introspect::Stage::OrderAck);
        fill_ = &book.series("orders", introspect::Stage::OrderFill);
    }

    void published(Channel channel, UnixNanos ts_event, UnixNanos ts_init) noexcept {
        const auto i = static_cast<std::size_t>(channel);
        const auto now = static_cast<std::int64_t>(clock_->wall().ns);
        const auto init = static_cast<std::int64_t>(ts_init.ns);
        internal_[i]->record(now - init);
        if (ts_event.ns == 0) {
            windows_[i].count(ts_init.ns);
            return;
        }
        const auto raw = init - static_cast<std::int64_t>(ts_event.ns);
        windows_[i].record(raw, ts_init.ns);
        feed_[i]->record(raw + correction_ns_);
    }

    void order_sent(std::uint64_t client_order_id) noexcept {
        sent_[client_order_id % k_orders] = Sent{
            .id = client_order_id, .wall_ns = clock_->wall().ns, .acked = false, .filled = false};
    }

    void order_event(const OrderEvent& event) noexcept {
        auto& slot = sent_[event.client_order_id.raw % k_orders];
        if (slot.id != event.client_order_id.raw || slot.wall_ns == 0) {
            return;
        }
        const auto elapsed = static_cast<std::int64_t>(clock_->wall().ns) - static_cast<std::int64_t>(slot.wall_ns);
        if (!slot.acked && event.type == OrderEventType::Accepted) {
            slot.acked = true;
            ack_->record(elapsed);
        } else if (!slot.filled && event.type == OrderEventType::Filled) {
            slot.filled = true;
            fill_->record(elapsed);
        }
    }

    void use_correction(std::int64_t ns) noexcept { correction_ns_ = ns; }

    // offset is at least this: no message arrives before the venue stamped it
    [[nodiscard]] std::optional<std::int64_t> feed_bound() const noexcept {
        std::optional<std::int64_t> out;
        for (const auto& window : windows_) {
            if (const auto least = window.lag_min(); least.has_value() && (!out.has_value() || -*least > *out)) {
                out = -*least;
            }
        }
        return out;
    }

    void advance(std::uint64_t mono_ns) noexcept {
        for (auto& window : windows_) {
            window.advance(mono_ns);
        }
    }

    [[nodiscard]] introspect::FeedSummary feed(std::size_t channel, std::uint64_t mono_ns) const noexcept {
        return windows_[channel].summary(mono_ns);
    }

private:
    static constexpr std::size_t k_orders = 256;

    struct Sent {
        std::uint64_t id{0};
        std::uint64_t wall_ns{0};
        bool acked{false};
        bool filled{false};
    };

    const Clock* clock_;
    std::array<introspect::LatencySeries*, k_metered> feed_{};
    std::array<introspect::LatencySeries*, k_metered> internal_{};
    introspect::LatencySeries* ack_{nullptr};
    introspect::LatencySeries* fill_{nullptr};
    std::array<introspect::FeedWindow, k_metered> windows_{};
    std::array<Sent, k_orders> sent_{};
    std::int64_t correction_ns_{0};
};

} // namespace raikiri::gatewayd
