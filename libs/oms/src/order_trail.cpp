#include <raikiri/domain/error_class.hpp>
#include <raikiri/oms/order_trail.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace raikiri::oms {

namespace internal {

// venue-minted text goes into a json string unchecked otherwise
template <std::size_t N> struct Clean {
    std::array<char, N> text{};
    std::size_t size{0};

    explicit Clean(std::string_view raw) noexcept : size{std::min(raw.size(), N)} {
        for (std::size_t i = 0; i < size; ++i) {
            const auto c = raw[i];
            text[i] = c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20 ? '?' : c;
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return {text.data(), size}; }
};

struct Who {
    std::array<char, 24> actor{};
    std::array<char, 40> subject{};
    std::size_t actor_size{0};
    std::size_t subject_size{0};

    explicit Who(ClientOrderId id) noexcept {
        const auto owner = id.strategy_id();
        actor_size = owner == k_manual_strategy_id ? fmt::format_to_n(actor.data(), actor.size(), "desk").size
                     : id.valid() ? fmt::format_to_n(actor.data(), actor.size(), "strategy:{}", owner).size
                                  : fmt::format_to_n(actor.data(), actor.size(), "venue").size;
        subject_size = fmt::format_to_n(subject.data(), subject.size(), "order:{}", id.raw).size;
    }

    [[nodiscard]] std::string_view actor_view() const noexcept { return {actor.data(), actor_size}; }

    [[nodiscard]] std::string_view subject_view() const noexcept { return {subject.data(), subject_size}; }
};

[[nodiscard]] constexpr std::string_view type_name(OrderType type) noexcept {
    return type == OrderType::Market ? "market" : "limit";
}

[[nodiscard]] constexpr std::string_view tif_name(TimeInForce tif) noexcept {
    return tif == TimeInForce::Ioc ? "ioc" : tif == TimeInForce::Fok ? "fok" : "gtc";
}

[[nodiscard]] constexpr std::int64_t or_zero(QtyLots lots) noexcept {
    return lots.is_undef() ? 0 : lots.raw;
}

[[nodiscard]] constexpr std::int64_t or_zero(PriceTicks price) noexcept {
    return price.is_undef() ? 0 : price.raw;
}

} // namespace internal

std::string_view trail_action(OrderEventType type) noexcept {
    using E = OrderEventType;
    switch (type) {
    case E::Initialized:
        return "order.created";
    case E::Denied:
        return "order.denied";
    case E::Submitted:
        return "order.sent";
    case E::Accepted:
        return "order.acked";
    case E::Rejected:
        return "order.rejected";
    case E::Triggered:
        return "order.triggered";
    case E::PendingUpdate:
        return "order.amend_sent";
    case E::Updated:
        return "order.amended";
    case E::ModifyRejected:
        return "order.amend_refused";
    case E::PendingCancel:
        return "order.cancel_sent";
    case E::Canceled:
        return "order.cancelled";
    case E::CancelRejected:
        return "order.cancel_refused";
    case E::Expired:
        return "order.expired";
    case E::Filled:
        return "order.filled";
    case E::ReconGap:
        return "order.recon_gap";
    case E::ReconClosed:
        return "order.recon_closed";
    }
    return "order.unknown";
}

void OrderTrail::command(std::string_view action, const OrderCmd& cmd, UnixNanos at) noexcept {
    const internal::Who who{cmd.client_order_id};
    std::array<char, 384> detail{};
    const auto wrote = fmt::format_to_n(
        detail.data(),
        detail.size(),
        R"({{"book":"{}","exchange_client_order_id":"{}","instrument":"{}","side":"{}",)"
        R"("type":"{}","tif":"{}","flags":{},"price_ticks":"{}","qty_lots":"{}"}})",
        cmd.client_order_id.strategy_id(),
        cmd.exchange_client_order_id.view(),
        cmd.instrument.packed,
        to_string(cmd.side),
        internal::type_name(cmd.order_type),
        internal::tif_name(cmd.tif),
        static_cast<std::uint8_t>(cmd.flags),
        internal::or_zero(cmd.price),
        internal::or_zero(cmd.qty));
    sink_->append({.ts = at,
                   .actor = who.actor_view(),
                   .action = action,
                   .subject = who.subject_view(),
                   .detail_json = {detail.data(), std::min(wrote.size, detail.size())}});
}

void OrderTrail::event(const OrderEvent& event, const Order* after, UnixNanos at) noexcept {
    const internal::Who who{event.client_order_id};
    const internal::Clean<40> venue_id{event.exchange_order_id.view()};
    const internal::Clean<40> exec_id{event.exec_id.view()};
    const internal::Clean<4> fee_asset{event.fee_asset.view()};
    std::array<char, 32> fee_text{};
    const std::string_view fee = event.fee.is_undef() || event.type != OrderEventType::Filled
                                     ? std::string_view{}
                                     : std::string_view{format_e9(event.fee, fee_text.data(), fee_text.size())};
    std::array<char, 640> detail{};
    const auto wrote = fmt::format_to_n(
        detail.data(),
        detail.size(),
        R"({{"book":"{}","exchange_client_order_id":"{}","exchange_order_id":"{}","instrument":"{}","side":"{}",)"
        R"("price_ticks":"{}","qty_lots":"{}","leaves_lots":"{}","filled_lots":"{}","exec_id":"{}","fee":"{}",)"
        R"("fee_asset":"{}","reject_code":{},"status":"{}","version":{},"ts_event":"{}"}})",
        event.client_order_id.strategy_id(),
        event.exchange_client_order_id.view(),
        venue_id.view(),
        event.instrument.packed,
        to_string(event.side),
        internal::or_zero(event.price),
        internal::or_zero(event.qty),
        after != nullptr ? internal::or_zero(after->leaves()) : internal::or_zero(event.leaves_qty),
        after != nullptr ? internal::or_zero(after->filled()) : 0,
        exec_id.view(),
        fee,
        fee_asset.view(),
        event.reject_code,
        after != nullptr ? to_string(after->status()) : std::string_view{"closed"},
        after != nullptr ? after->version() : 0,
        event.ts_event.ns);
    sink_->append({.ts = at,
                   .actor = who.actor_view(),
                   .action = trail_action(event.type),
                   .subject = who.subject_view(),
                   .detail_json = {detail.data(), std::min(wrote.size, detail.size())}});
}

} // namespace raikiri::oms
