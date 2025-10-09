#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::kucoin {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

OrderChangeType change_from(std::string_view type) noexcept {
    if (type == "open") {
        return OrderChangeType::Open;
    }
    if (type == "match") {
        return OrderChangeType::Match;
    }
    if (type == "filled") {
        return OrderChangeType::Filled;
    }
    if (type == "canceled" || type == "cancel") {
        return OrderChangeType::Canceled;
    }
    if (type == "update") {
        return OrderChangeType::Update;
    }
    return OrderChangeType::Unknown;
}

// Both hosts spell the shared order fields the same way; what differs is which
// of them arrive, so every one is optional and the mapper decides what it needs.
std::expected<void, WireError> read_order(detail::ondemand::object& obj, WireOrder& out) {
    const auto symbol = req_decimal(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected(symbol.error());
    }
    out.symbol = *symbol;
    for (const auto& [key, field] : {std::pair{"id", &out.order_id},
                                     std::pair{"clientOid", &out.client_oid},
                                     std::pair{"side", &out.side},
                                     std::pair{"type", &out.order_type},
                                     std::pair{"price", &out.price},
                                     std::pair{"size", &out.size},
                                     std::pair{"dealSize", &out.deal_size},
                                     std::pair{"remainSize", &out.remain_size},
                                     std::pair{"status", &out.status}}) {
        const auto value = opt_decimal(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    // Spot says `active` and `lastUpdatedAt`, futures says `isActive` and
    // `updatedAt`; reading only one pair loses half the answer.
    const auto active = detail::opt_bool(obj, "active");
    const auto is_active = detail::opt_bool(obj, "isActive");
    const auto cancel_exist = detail::opt_bool(obj, "cancelExist");
    if (!active.has_value() || !is_active.has_value() || !cancel_exist.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.active = *active || *is_active;
    out.cancel_exist = *cancel_exist;
    const auto updated = opt_u64(obj, "updatedAt");
    const auto last_updated = opt_u64(obj, "lastUpdatedAt");
    if (!updated.has_value() || !last_updated.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.updated_ms = *updated != 0 ? *updated : *last_updated;
    return {};
}

std::expected<void, WireError> read_fill(detail::ondemand::object& obj, WireFill& out) {
    for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                     std::pair{"tradeId", &out.trade_id},
                                     std::pair{"orderId", &out.order_id},
                                     std::pair{"side", &out.side},
                                     std::pair{"liquidity", &out.liquidity},
                                     std::pair{"price", &out.price},
                                     std::pair{"size", &out.size},
                                     std::pair{"fee", &out.fee}}) {
        const auto value = opt_decimal(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    const auto created = opt_u64(obj, "createdAt");
    if (!created.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.created_ms = *created;
    return {};
}

} // namespace

std::expected<WireOrderChange, WireError> parse_order_change(std::string_view frame) {
    return guarded([&]() -> std::expected<WireOrderChange, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireOrderChange, WireError> {
                WireOrderChange out{};
                for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                                 std::pair{"orderId", &out.order_id},
                                                 std::pair{"clientOid", &out.client_oid},
                                                 std::pair{"tradeId", &out.trade_id},
                                                 std::pair{"side", &out.side},
                                                 std::pair{"orderType", &out.order_type},
                                                 std::pair{"price", &out.price},
                                                 std::pair{"size", &out.size},
                                                 std::pair{"filledSize", &out.filled_size},
                                                 std::pair{"matchPrice", &out.match_price},
                                                 std::pair{"matchSize", &out.match_size},
                                                 std::pair{"remainSize", &out.remain_size},
                                                 std::pair{"canceledSize", &out.canceled_size},
                                                 std::pair{"oldSize", &out.old_size},
                                                 std::pair{"liquidity", &out.liquidity}}) {
                    const auto value = opt_decimal(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *field = *value;
                }
                if (out.symbol.empty()) {
                    return std::unexpected(WireError::MissingField);
                }
                const auto type = req_decimal(obj, "type");
                if (!type.has_value()) {
                    return std::unexpected(type.error());
                }
                out.type = change_from(*type);
                // `orderTime` counts milliseconds on spot and nanoseconds on
                // futures, so it is carried raw and never timed by.
                const auto order_time = opt_u64(obj, "orderTime");
                const auto ts = opt_u64(obj, "ts");
                if (!order_time.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.order_time = *order_time;
                out.ts_ns = *ts;
                return out;
            });
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_spot_orders(std::string_view body,
                                                                       OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WireOrder>(
            *array, storage.orders, [](auto& obj, WireOrder& row) { return read_order(obj, row); });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireOrder>{storage.orders.data(), *count};
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_futures_orders(std::string_view body,
                                                                          OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(*root,
                         [&](detail::ondemand::object& obj)
                             -> std::expected<std::span<const WireOrder>, WireError> {
                             auto array = detail::req_array(obj, "items");
                             if (!array.has_value()) {
                                 return std::unexpected(array.error());
                             }
                             const auto count = read_rows<WireOrder>(
                                 *array, storage.orders, [](auto& row_obj, WireOrder& row) {
                                     return read_order(row_obj, row);
                                 });
                             if (!count.has_value()) {
                                 return std::unexpected(count.error());
                             }
                             return std::span<const WireOrder>{storage.orders.data(), *count};
                         });
    });
}

std::expected<std::span<const std::string_view>, WireError>
parse_active_symbols(std::string_view body, SymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const std::string_view>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(*root,
                         [&](detail::ondemand::object& obj)
                             -> std::expected<std::span<const std::string_view>, WireError> {
                             auto array = detail::req_array(obj, "symbols");
                             if (!array.has_value()) {
                                 return std::unexpected(array.error());
                             }
                             std::size_t count = 0;
                             for (auto entry : *array) {
                                 if (count == storage.symbols.size()) {
                                     return std::unexpected(WireError::TooManyEntries);
                                 }
                                 detail::ondemand::value value;
                                 if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                                     return std::unexpected(detail::map_error(ec));
                                 }
                                 const auto text = detail::str_of(value);
                                 if (!text.has_value()) {
                                     return std::unexpected(text.error());
                                 }
                                 storage.symbols[count] = *text;
                                 ++count;
                             }
                             return std::span<const std::string_view>{storage.symbols.data(),
                                                                      count};
                         });
    });
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                FillBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFill>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(*root,
                         [&](detail::ondemand::object& obj)
                             -> std::expected<std::span<const WireFill>, WireError> {
                             auto array = detail::req_array(obj, "items");
                             if (!array.has_value()) {
                                 return std::unexpected(array.error());
                             }
                             const auto count = read_rows<WireFill>(
                                 *array, storage.fills, [](auto& row_obj, WireFill& row) {
                                     return read_fill(row_obj, row);
                                 });
                             if (!count.has_value()) {
                                 return std::unexpected(count.error());
                             }
                             return std::span<const WireFill>{storage.fills.data(), *count};
                         });
    });
}

} // namespace raikiri::venues::kucoin
