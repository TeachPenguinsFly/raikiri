#include "wire_detail.hpp"

namespace raikiri::venues::kucoin {

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
    const auto symbol = detail::req_decimal(obj, "symbol");
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
        const auto value = detail::opt_decimal(obj, key);
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
    const auto updated = detail::opt_u64(obj, "updatedAt");
    const auto last_updated = detail::opt_u64(obj, "lastUpdatedAt");
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
        const auto value = detail::opt_decimal(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    const auto created = detail::opt_u64(obj, "createdAt");
    if (!created.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.created_ms = *created;
    return {};
}

// Walks `items` inside the paged `data` object, or `data` itself where KuCoin
// returns the array unwrapped.
template <typename Row, typename Read>
std::expected<std::size_t, WireError>
read_rows(detail::ondemand::array array, std::span<Row> into, Read read) {
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        Row row{};
        if (const auto ok = read(obj, row); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = row;
        ++count;
    }
    return count;
}

} // namespace

std::expected<EventInfo, WireError> parse_private_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = detail::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        EventInfo info{};
        if (*type != "message") {
            if (*type == "welcome") {
                info.kind = EventKind::Welcome;
            } else if (*type == "ack") {
                info.kind = EventKind::Ack;
            } else if (*type == "pong") {
                info.kind = EventKind::Pong;
            } else if (*type == "error") {
                info.kind = EventKind::Error;
            }
            return info;
        }
        const auto subject = detail::req_str(*root, "subject");
        if (!subject.has_value()) {
            return std::unexpected(subject.error());
        }
        // The all-symbols topic says orderChange; the per-symbol form of the
        // same futures topic says symbolOrderChange, and the payloads match.
        if (*subject == "orderChange" || *subject == "symbolOrderChange") {
            info.kind = EventKind::OrderChange;
        }
        return info;
    });
}

std::expected<WireOrderChange, WireError> parse_order_change(std::string_view frame) {
    return guarded([&]() -> std::expected<WireOrderChange, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_data(
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
                    const auto value = detail::opt_decimal(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *field = *value;
                }
                if (out.symbol.empty()) {
                    return std::unexpected(WireError::MissingField);
                }
                const auto type = detail::req_decimal(obj, "type");
                if (!type.has_value()) {
                    return std::unexpected(type.error());
                }
                out.type = change_from(*type);
                // `orderTime` counts milliseconds on spot and nanoseconds on
                // futures, so it is carried raw and never timed by.
                const auto order_time = detail::opt_u64(obj, "orderTime");
                const auto ts = detail::opt_u64(obj, "ts");
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
        const auto count =
            read_rows<WireOrder>(*array, storage.orders, [](auto& obj, WireOrder& row) {
                return read_order(obj, row);
            });
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
        return detail::with_data(
            *root,
            [&](detail::ondemand::object& obj)
                -> std::expected<std::span<const WireOrder>, WireError> {
                auto array = detail::req_array(obj, "items");
                if (!array.has_value()) {
                    return std::unexpected(array.error());
                }
                const auto count =
                    read_rows<WireOrder>(*array, storage.orders, [](auto& row_obj, WireOrder& row) {
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
        return detail::with_data(
            *root,
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
                return std::span<const std::string_view>{storage.symbols.data(), count};
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
        return detail::with_data(
            *root,
            [&](detail::ondemand::object& obj)
                -> std::expected<std::span<const WireFill>, WireError> {
                auto array = detail::req_array(obj, "items");
                if (!array.has_value()) {
                    return std::unexpected(array.error());
                }
                const auto count =
                    read_rows<WireFill>(*array, storage.fills, [](auto& row_obj, WireFill& row) {
                        return read_fill(row_obj, row);
                    });
                if (!count.has_value()) {
                    return std::unexpected(count.error());
                }
                return std::span<const WireFill>{storage.fills.data(), *count};
            });
    });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                         PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WirePosition>(
            *array,
            storage.positions,
            [](auto& obj, WirePosition& row) -> std::expected<void, WireError> {
                const auto symbol = detail::req_decimal(obj, "symbol");
                const auto qty = detail::opt_decimal(obj, "currentQty");
                const auto entry = detail::opt_decimal(obj, "avgEntryPrice");
                const auto ts = detail::opt_u64(obj, "currentTimestamp");
                if (!symbol.has_value() || !qty.has_value() || !entry.has_value() ||
                    !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                row.symbol = *symbol;
                row.current_qty = *qty;
                row.avg_entry_price = *entry;
                row.ts_ms = *ts;
                return {};
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WirePosition>{storage.positions.data(), *count};
    });
}

std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotSymbol>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WireSpotSymbol>(
            *array,
            storage.symbols,
            [](auto& obj, WireSpotSymbol& row) -> std::expected<void, WireError> {
                const auto symbol = detail::req_decimal(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                row.symbol = *symbol;
                for (const auto& [key, field] :
                     {std::pair{"baseIncrement", &row.base_increment},
                      std::pair{"priceIncrement", &row.price_increment},
                      std::pair{"minFunds", &row.min_funds}}) {
                    const auto value = detail::opt_decimal(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *field = *value;
                }
                const auto trading = detail::opt_bool(obj, "enableTrading");
                if (!trading.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                row.enable_trading = *trading;
                return {};
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), *count};
    });
}

} // namespace raikiri::venues::kucoin
