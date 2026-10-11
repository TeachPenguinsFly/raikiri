#include <raikiri/venues/nado/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <optional>

namespace raikiri::venues::nado {

namespace {

using wire::guarded;
using wire::session;

// stamps and ids come as json strings of digits or as numbers
std::expected<std::uint64_t, WireError> u64_text(wire::ondemand::value& value) {
    wire::ondemand::json_type type{};
    if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    if (type != wire::ondemand::json_type::string) {
        return wire::u64_of(value);
    }
    const auto text = wire::str_of(value);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    if (text->empty() || text->size() > 20 || !std::ranges::all_of(*text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::unexpected(WireError::WrongType);
    }
    return wire::digits_to_u64(*text);
}

// an x18 integer, possibly signed, as the string the venue sends
std::expected<std::string_view, WireError> x18_text(wire::ondemand::value& value) {
    const auto text = wire::str_of(value);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    const auto digits = text->starts_with('-') ? text->substr(1) : *text;
    if (digits.empty() || !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::unexpected(WireError::WrongType);
    }
    return *text;
}

std::expected<std::size_t, WireError> read_levels(wire::ondemand::value& side, std::span<WireLevel> into) {
    wire::ondemand::array array;
    if (const auto ec = side.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        std::size_t field = 0;
        for (auto element : pair) {
            wire::ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            if (field < 2) {
                const auto text = x18_text(value);
                if (!text.has_value()) {
                    return std::unexpected(text.error());
                }
                (field == 0 ? into[count].price : into[count].qty) = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected(WireError::MissingField);
        }
        ++count;
    }
    return count;
}

// the push and the snapshot share a body: bids, asks, product_id and stamps
std::expected<WireDepth, WireError> read_book(wire::ondemand::object& obj, LevelBuffer& storage, bool snapshot) {
    WireDepth out{};
    std::optional<std::size_t> bids;
    std::optional<std::size_t> asks;
    bool product = false;
    bool stamped = false;
    const auto walked = wire::for_each_field(
        obj, [&](std::string_view key, wire::ondemand::value& value) -> std::expected<void, WireError> {
            if (key == "bids" || key == "asks") {
                const bool bid = key == "bids";
                const auto count = read_levels(value, bid ? std::span<WireLevel>{storage.bids}
                                                          : std::span<WireLevel>{storage.asks});
                if (!count.has_value()) {
                    return std::unexpected(count.error());
                }
                (bid ? bids : asks) = *count;
                return {};
            }
            std::uint64_t* target = nullptr;
            if (key == "product_id") {
                const auto id = wire::u64_of(value);
                if (!id.has_value()) {
                    return std::unexpected(id.error());
                }
                out.product_id = static_cast<std::uint32_t>(*id);
                product = true;
                return {};
            }
            if (key == (snapshot ? "timestamp" : "max_timestamp")) {
                target = &out.max_ts;
                stamped = true;
            } else if (!snapshot && key == "last_max_timestamp") {
                target = &out.last_max_ts;
            }
            if (target != nullptr) {
                const auto stamp = u64_text(value);
                if (!stamp.has_value()) {
                    return std::unexpected(stamp.error());
                }
                *target = *stamp;
            }
            return {};
        });
    if (!walked.has_value()) {
        return std::unexpected(walked.error());
    }
    if (!bids.has_value() || !asks.has_value() || !product || !stamped) {
        return std::unexpected(WireError::MissingField);
    }
    out.bids = {storage.bids.data(), *bids};
    out.asks = {storage.asks.data(), *asks};
    return out;
}

} // namespace

std::expected<std::span<const WireSymbol>, WireError> parse_symbols(std::string_view body, SymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSymbol>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto symbols = wire::req_object(*data, "symbols");
        if (!symbols.has_value()) {
            return std::unexpected(symbols.error());
        }
        std::size_t count = 0;
        const auto walked = wire::for_each_field(
            *symbols, [&](std::string_view, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (count == storage.rows.size()) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                wire::ondemand::object obj;
                if (const auto ec = value.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                WireSymbol out{};
                bool product = false;
                const auto fields = wire::for_each_field(
                    obj, [&](std::string_view key, wire::ondemand::value& field) -> std::expected<void, WireError> {
                        if (key == "product_id") {
                            const auto id = wire::u64_of(field);
                            if (!id.has_value()) {
                                return std::unexpected(id.error());
                            }
                            out.product_id = static_cast<std::uint32_t>(*id);
                            product = true;
                            return {};
                        }
                        std::string_view* text = key == "symbol"                ? &out.symbol
                                                 : key == "type"                ? &out.type
                                                 : key == "trading_status"      ? &out.status
                                                 : key == "price_increment_x18" ? &out.tick_x18
                                                 : key == "size_increment"      ? &out.lot_x18
                                                 : key == "min_size"            ? &out.min_notional_x18
                                                 : key == "maker_fee_rate_x18"  ? &out.maker_fee_x18
                                                 : key == "taker_fee_rate_x18"  ? &out.taker_fee_x18
                                                                                : nullptr;
                        if (text != nullptr) {
                            const auto read = wire::str_of(field);
                            if (!read.has_value()) {
                                return std::unexpected(read.error());
                            }
                            *text = *read;
                        }
                        return {};
                    });
                if (!fields.has_value()) {
                    return std::unexpected(fields.error());
                }
                if (!product || out.symbol.empty() || out.type.empty() || out.tick_x18.empty() || out.lot_x18.empty()) {
                    return std::unexpected(WireError::MissingField);
                }
                storage.rows[count++] = out;
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        return std::span<const WireSymbol>{storage.rows.data(), count};
    });
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        EventInfo info{};
        bool reply = false;
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view key, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "type") {
                    const auto type = wire::str_of(value);
                    if (!type.has_value()) {
                        return std::unexpected(type.error());
                    }
                    info.kind = *type == "best_bid_offer" ? EventKind::Bbo
                                : *type == "trade"        ? EventKind::Trade
                                : *type == "book_depth"   ? EventKind::Depth
                                : *type == "funding_rate" ? EventKind::Funding
                                                          : EventKind::Unknown;
                } else if (key == "id") {
                    reply = true;
                } else if (key == "error") {
                    info.refused = true;
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (info.kind == EventKind::Unknown && reply) {
            info.kind = EventKind::Reply;
        }
        return info;
    });
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireBbo out{};
        bool product = false;
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view key, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "timestamp") {
                    const auto stamp = u64_text(value);
                    if (!stamp.has_value()) {
                        return std::unexpected(stamp.error());
                    }
                    out.ts_ns = *stamp;
                    return {};
                }
                if (key == "product_id") {
                    const auto id = wire::u64_of(value);
                    if (!id.has_value()) {
                        return std::unexpected(id.error());
                    }
                    out.product_id = static_cast<std::uint32_t>(*id);
                    product = true;
                    return {};
                }
                std::string_view* text = key == "bid_price"   ? &out.bid_price
                                         : key == "bid_qty"   ? &out.bid_qty
                                         : key == "ask_price" ? &out.ask_price
                                         : key == "ask_qty"   ? &out.ask_qty
                                                              : nullptr;
                if (text != nullptr) {
                    const auto read = x18_text(value);
                    if (!read.has_value()) {
                        return std::unexpected(read.error());
                    }
                    *text = *read;
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (!product || out.bid_price.empty() || out.bid_qty.empty() || out.ask_price.empty() || out.ask_qty.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<WireTrade, WireError> parse_trade(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTrade, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTrade out{};
        bool product = false;
        bool side = false;
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view key, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "timestamp") {
                    const auto stamp = u64_text(value);
                    if (!stamp.has_value()) {
                        return std::unexpected(stamp.error());
                    }
                    out.ts_ns = *stamp;
                } else if (key == "product_id") {
                    const auto id = wire::u64_of(value);
                    if (!id.has_value()) {
                        return std::unexpected(id.error());
                    }
                    out.product_id = static_cast<std::uint32_t>(*id);
                    product = true;
                } else if (key == "price" || key == "taker_qty") {
                    const auto read = x18_text(value);
                    if (!read.has_value()) {
                        return std::unexpected(read.error());
                    }
                    (key == "price" ? out.price : out.qty) = *read;
                } else if (key == "is_taker_buyer") {
                    bool buyer = false;
                    if (const auto ec = value.get_bool().get(buyer); ec != simdjson::SUCCESS) {
                        return std::unexpected(wire::map_error(ec));
                    }
                    out.taker_bought = buyer;
                    side = true;
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (!product || !side || out.price.empty() || out.qty.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_book(*root, storage, /*snapshot=*/false);
    });
}

std::expected<WireDepth, WireError> parse_liquidity(std::string_view body, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        return read_book(*data, storage, /*snapshot=*/true);
    });
}

std::expected<WireFunding, WireError> parse_funding(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFunding, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireFunding out{};
        bool product = false;
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view key, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "timestamp" || key == "update_time") {
                    const auto stamp = u64_text(value);
                    if (!stamp.has_value()) {
                        return std::unexpected(stamp.error());
                    }
                    (key == "timestamp" ? out.ts_ns : out.update_s) = *stamp;
                } else if (key == "product_id") {
                    const auto id = wire::u64_of(value);
                    if (!id.has_value()) {
                        return std::unexpected(id.error());
                    }
                    out.product_id = static_cast<std::uint32_t>(*id);
                    product = true;
                } else if (key == "funding_rate_x18") {
                    const auto read = x18_text(value);
                    if (!read.has_value()) {
                        return std::unexpected(read.error());
                    }
                    out.rate_x18 = *read;
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (!product || out.rate_x18.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                      ContractBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (count == storage.rows.size()) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                wire::ondemand::object obj;
                if (const auto ec = value.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                WireContract out{};
                bool product = false;
                const auto fields = wire::for_each_field(
                    obj, [&](std::string_view key, wire::ondemand::value& field) -> std::expected<void, WireError> {
                        if (key == "product_id") {
                            const auto id = wire::u64_of(field);
                            if (!id.has_value()) {
                                return std::unexpected(id.error());
                            }
                            out.product_id = static_cast<std::uint32_t>(*id);
                            product = true;
                            return {};
                        }
                        std::string_view* text = key == "mark_price"      ? &out.mark_price
                                                 : key == "index_price"   ? &out.index_price
                                                 : key == "open_interest" ? &out.open_interest
                                                 : key == "base_volume"   ? &out.base_volume
                                                 : key == "quote_volume"  ? &out.quote_volume
                                                                          : nullptr;
                        if (text != nullptr) {
                            const auto raw = wire::raw_of(field);
                            if (!raw.has_value()) {
                                return std::unexpected(raw.error());
                            }
                            *text = *raw;
                        }
                        return {};
                    });
                if (!fields.has_value()) {
                    return std::unexpected(fields.error());
                }
                if (!product) {
                    return std::unexpected(WireError::MissingField);
                }
                storage.rows[count++] = out;
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        return std::span<const WireContract>{storage.rows.data(), count};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time_ms(std::string_view body) {
    while (!body.empty() && (body.back() == '\n' || body.back() == ' ')) {
        body.remove_suffix(1);
    }
    if (body.empty() || body.size() > 20 || !std::ranges::all_of(body, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::unexpected(WireError::WrongType);
    }
    return wire::digits_to_u64(body);
}

} // namespace raikiri::venues::nado
