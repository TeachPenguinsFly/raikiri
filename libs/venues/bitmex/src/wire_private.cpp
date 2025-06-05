#include <utility>

#include "wire_detail.hpp"

namespace raikiri::venues::bitmex {

namespace {

using detail::guarded;
using detail::session;

// REST answers a bare JSON array of rows; the websocket wraps the SAME rows in
// {"table":..,"action":..,"data":[..]}. Reading the first non-space byte is
// what lets one reader serve both, and a row read out of the wrong wrapper is
// the failure this avoids — an empty result that looks like an empty account.
template <typename Row, typename Read>
[[nodiscard]] std::expected<std::span<const Row>, WireError>
read_rows(std::string_view payload, std::span<Row> into, Read&& read) {
    return guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        std::size_t at = 0;
        while (at < payload.size() && (payload[at] == ' ' || payload[at] == '\n' ||
                                       payload[at] == '\r' || payload[at] == '\t')) {
            ++at;
        }
        if (at == payload.size()) {
            return std::unexpected(WireError::NotJson);
        }
        std::optional<detail::ondemand::array> array;
        if (payload[at] == '[') {
            auto bare = session().open_array(payload);
            if (!bare.has_value()) {
                return std::unexpected(bare.error());
            }
            array = *bare;
        } else {
            auto root = session().open_object(payload);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            auto wrapped = detail::req_array(*root, "data");
            if (!wrapped.has_value()) {
                return std::unexpected(wrapped.error());
            }
            array = *wrapped;
        }

        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == into.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto row = read(obj);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            into[count] = *row;
            ++count;
        }
        return std::span<const Row>{into.data(), count};
    });
}

// Every string column but the row key is optional: an `update` action carries
// only what changed, and BitMEX keys `order` on orderID alone.
[[nodiscard]] std::expected<void, WireError>
read_strings(detail::ondemand::object& obj,
             std::initializer_list<std::pair<const char*, std::string_view*>> fields) {
    for (const auto& [key, out] : fields) {
        const auto value = detail::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *out = *value;
    }
    return {};
}

[[nodiscard]] std::expected<void, WireError>
read_numbers(detail::ondemand::object& obj,
             std::initializer_list<std::pair<const char*, std::string_view*>> fields) {
    for (const auto& [key, out] : fields) {
        const auto value = detail::opt_num(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *out = *value;
    }
    return {};
}

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

[[nodiscard]] std::expected<WirePosition, WireError> read_position(detail::ondemand::object& obj) {
    WirePosition out{};
    const auto symbol = detail::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected(symbol.error());
    }
    out.symbol = *symbol;
    const auto strings = read_strings(obj, {{"timestamp", &out.timestamp}});
    if (!strings.has_value()) {
        return std::unexpected(strings.error());
    }
    const auto numbers = read_numbers(
        obj, {{"currentQty", &out.current_qty}, {"avgEntryPrice", &out.avg_entry_price}});
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

std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view payload, PositionBuffer& storage) {
    return read_rows<WirePosition>(payload, storage.positions, read_position);
}

} // namespace raikiri::venues::bitmex
