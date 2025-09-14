#include <raikiri/venues/bitmex/envelope.hpp>
#include <raikiri/venues/bitmex/orders.hpp>

namespace raikiri::venues::bitmex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

[[nodiscard]] std::expected<WireOrder, WireError> read_order(detail::ondemand::object& obj) {
    WireOrder out{};
    const auto order_id = detail::req_str(obj, "orderID");
    if (!order_id.has_value()) {
        return std::unexpected(order_id.error());
    }
    out.order_id = *order_id;
    const auto strings = read_strings(obj,
                                      {{"clOrdID", &out.cl_ord_id},
                                       {"symbol", &out.symbol},
                                       {"side", &out.side},
                                       {"ordStatus", &out.ord_status},
                                       {"ordType", &out.ord_type},
                                       {"timeInForce", &out.time_in_force},
                                       {"execInst", &out.exec_inst},
                                       {"ordRejReason", &out.ord_rej_reason},
                                       {"text", &out.text},
                                       {"error", &out.error},
                                       {"transactTime", &out.transact_time},
                                       {"timestamp", &out.timestamp}});
    if (!strings.has_value()) {
        return std::unexpected(strings.error());
    }
    const auto numbers = read_numbers(obj,
                                      {{"price", &out.price},
                                       {"orderQty", &out.order_qty},
                                       {"leavesQty", &out.leaves_qty},
                                       {"cumQty", &out.cum_qty}});
    if (!numbers.has_value()) {
        return std::unexpected(numbers.error());
    }
    return out;
}

[[nodiscard]] std::expected<WireExecution, WireError>
read_execution(detail::ondemand::object& obj) {
    WireExecution out{};
    const auto exec_id = detail::req_str(obj, "execID");
    if (!exec_id.has_value()) {
        return std::unexpected(exec_id.error());
    }
    out.exec_id = *exec_id;
    const auto strings = read_strings(obj,
                                      {{"orderID", &out.order_id},
                                       {"clOrdID", &out.cl_ord_id},
                                       {"symbol", &out.symbol},
                                       {"side", &out.side},
                                       {"execType", &out.exec_type},
                                       {"ordStatus", &out.ord_status},
                                       {"lastLiquidityInd", &out.last_liquidity_ind},
                                       {"transactTime", &out.transact_time}});
    if (!strings.has_value()) {
        return std::unexpected(strings.error());
    }
    const auto numbers = read_numbers(obj,
                                      {{"lastQty", &out.last_qty},
                                       {"lastPx", &out.last_px},
                                       {"leavesQty", &out.leaves_qty},
                                       {"cumQty", &out.cum_qty},
                                       {"commission", &out.commission}});
    if (!numbers.has_value()) {
        return std::unexpected(numbers.error());
    }
    return out;
}

} // namespace

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view payload,
                                                                  OrderBuffer& storage) {
    return read_rows<WireOrder>(payload, storage.orders, read_order);
}

std::expected<WireOrder, WireError> parse_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_order(*root);
    });
}

std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view payload, ExecutionBuffer& storage) {
    return read_rows<WireExecution>(payload, storage.executions, read_execution);
}

} // namespace raikiri::venues::bitmex
