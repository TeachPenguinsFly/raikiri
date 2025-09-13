#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/orders.hpp>

namespace raikiri::venues::okx {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<void, WireError> read_order(detail::ondemand::object& obj, WireOrder& out) {
    const auto inst_id = detail::req_str(obj, "instId");
    if (!inst_id.has_value()) {
        return std::unexpected(inst_id.error());
    }
    out.inst_id = *inst_id;

    std::string_view state;
    std::string_view code;
    if (const auto ok = read_strings(obj,
                                     {{"ordId", &out.ord_id},
                                      {"clOrdId", &out.cl_ord_id},
                                      {"side", &out.side},
                                      {"ordType", &out.ord_type},
                                      {"px", &out.px},
                                      {"sz", &out.sz},
                                      {"accFillSz", &out.acc_fill_sz},
                                      {"avgPx", &out.avg_px},
                                      {"fillPx", &out.fill_px},
                                      {"fillSz", &out.fill_sz},
                                      {"fillFee", &out.fill_fee},
                                      {"tradeId", &out.trade_id},
                                      {"execType", &out.exec_type},
                                      {"cancelSource", &out.cancel_source},
                                      {"amendResult", &out.amend_result},
                                      {"state", &state},
                                      {"code", &code}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    out.state = order_state_from(state);
    out.code = static_cast<std::int64_t>(detail::digits_to_u64(code));

    const auto fill_ms = detail::opt_ms(obj, "fillTime");
    const auto u_ms = detail::opt_ms(obj, "uTime");
    if (!fill_ms.has_value() || !u_ms.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.fill_ms = *fill_ms;
    out.u_ms = *u_ms;
    return {};
}

std::expected<void, WireError> read_fill(detail::ondemand::object& obj, WireFill& out) {
    const auto inst_id = detail::req_str(obj, "instId");
    if (!inst_id.has_value()) {
        return std::unexpected(inst_id.error());
    }
    out.inst_id = *inst_id;
    if (const auto ok = read_strings(obj,
                                     {{"tradeId", &out.trade_id},
                                      {"ordId", &out.ord_id},
                                      {"clOrdId", &out.cl_ord_id},
                                      {"side", &out.side},
                                      {"fillPx", &out.fill_px},
                                      {"fillSz", &out.fill_sz},
                                      {"fee", &out.fee},
                                      {"execType", &out.exec_type}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    const auto fill_ms = detail::opt_ms(obj, "fillTime");
    if (!fill_ms.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.fill_ms = *fill_ms;
    return {};
}

std::expected<void, WireError> read_ack(detail::ondemand::object& obj, WireOrderAck& out) {
    if (const auto ok = read_strings(
            obj, {{"ordId", &out.ord_id}, {"clOrdId", &out.cl_ord_id}, {"sMsg", &out.s_msg}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    // sCode is the per-order verdict and is REQUIRED: an object that does not
    // carry one must never be read as the "0" that means the order is live.
    const auto s_code = detail::req_str(obj, "sCode");
    if (!s_code.has_value()) {
        return std::unexpected(s_code.error());
    }
    const auto value = detail::digits_to_u64(*s_code);
    if (value == 0 && *s_code != "0") {
        return std::unexpected(WireError::WrongType);
    }
    out.s_code = static_cast<std::int64_t>(value);
    return {};
}

} // namespace

WireOrderState order_state_from(std::string_view text) noexcept {
    if (text == "live") {
        return WireOrderState::Live;
    }
    if (text == "partially_filled") {
        return WireOrderState::PartiallyFilled;
    }
    if (text == "filled") {
        return WireOrderState::Filled;
    }
    if (text == "canceled") {
        return WireOrderState::Canceled;
    }
    if (text == "mmp_canceled") {
        return WireOrderState::MmpCanceled;
    }
    return WireOrderState::Unknown;
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                  OrderBuffer& storage) {
    return guarded([&] { return read_data<WireOrder>(body, storage.orders, read_order); });
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                FillBuffer& storage) {
    return guarded([&] { return read_data<WireFill>(body, storage.fills, read_fill); });
}

std::expected<std::span<const WireOrderAck>, WireError> parse_order_acks(std::string_view body,
                                                                         AckBuffer& storage) {
    return guarded([&] { return read_data<WireOrderAck>(body, storage.acks, read_ack); });
}

} // namespace raikiri::venues::okx
