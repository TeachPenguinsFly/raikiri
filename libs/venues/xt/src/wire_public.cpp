#include "wire_detail.hpp"

namespace raikiri::venues::xt {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_topic(std::string_view topic) noexcept {
    if (topic == "depth") {
        return EventKind::Depth;
    }
    if (topic == "depth_update") {
        return EventKind::DepthUpdate;
    }
    if (topic == "trade") {
        return EventKind::Trade;
    }
    if (topic == "mark_price") {
        return EventKind::MarkPrice;
    }
    return EventKind::Unknown;
}

// A level is ["64152.1","71044"]: price and a count of contracts. XT appends
// nothing after those two, unlike okx's four-element form.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto text = detail::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            (field == 0 ? level.price : level.qty) = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

// Every channel wraps its payload in a `data` OBJECT — there is no array to
// index and no empty-array case to guard, unlike okx.
template <typename Fn>
auto with_data(detail::ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<detail::ondemand::object&>())) {
    auto data = detail::req_object(root, "data");
    if (!data.has_value()) {
        return std::unexpected(data.error());
    }
    return std::forward<Fn>(fn)(*data);
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto topic = detail::opt_str(*root, "topic");
        if (!topic.has_value()) {
            return std::unexpected(topic.error());
        }
        EventInfo info{};
        if (topic->empty()) {
            const auto code = detail::req_i64(*root, "code");
            info.kind = !code.has_value() ? EventKind::Unknown
                        : *code == k_ok   ? EventKind::Ack
                                          : EventKind::Error;
            return info;
        }
        info.kind = kind_from_topic(*topic);
        return with_data(
            *root, [&](detail::ondemand::object& data) -> std::expected<EventInfo, WireError> {
                const auto symbol = detail::req_str(data, "s");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                info.symbol = *symbol;
                return info;
            });
    });
}

std::expected<StreamAck, WireError> parse_stream_ack(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamAck, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::req_i64(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        const auto id = detail::opt_str(*root, "id");
        if (!code.has_value() || !msg.has_value() || !id.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamAck{.code = *code, .msg = *msg, .id = *id};
    });
}

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [&](detail::ondemand::object& data) -> std::expected<WireDepth, WireError> {
                WireDepth out{};
                const auto symbol = detail::req_str(data, "s");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                const auto update_id = detail::req_digits(data, "id");
                if (!update_id.has_value()) {
                    return std::unexpected(update_id.error());
                }
                out.update_id = *update_id;
                const auto bids = read_levels(data, "b", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(data, "a", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto ts = detail::opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                out.bids = {storage.bids.data(), *bids};
                out.asks = {storage.asks.data(), *asks};
                return out;
            });
    });
}

std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view frame,
                                                             LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepthUpdate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root,
            [&](detail::ondemand::object& data) -> std::expected<WireDepthUpdate, WireError> {
                WireDepthUpdate out{};
                const auto symbol = detail::req_str(data, "s");
                const auto first = detail::req_digits(data, "fu");
                const auto prev = detail::req_digits(data, "pu");
                const auto update_id = detail::req_digits(data, "u");
                if (!symbol.has_value() || !first.has_value() || !prev.has_value() ||
                    !update_id.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.symbol = *symbol;
                out.first_update_id = *first;
                out.prev_update_id = *prev;
                out.update_id = *update_id;
                const auto bids = read_levels(data, "b", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(data, "a", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto ts = detail::opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                out.bids = {storage.bids.data(), *bids};
                out.asks = {storage.asks.data(), *asks};
                return out;
            });
    });
}

std::expected<WireTrade, WireError> parse_trade(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTrade, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(*root,
                         [](detail::ondemand::object& data) -> std::expected<WireTrade, WireError> {
                             WireTrade out{};
                             const auto symbol = detail::req_str(data, "s");
                             const auto price = detail::req_str(data, "p");
                             const auto qty = detail::req_str(data, "a");
                             const auto side = detail::req_str(data, "m");
                             if (!symbol.has_value() || !price.has_value() || !qty.has_value() ||
                                 !side.has_value()) {
                                 return std::unexpected(WireError::MissingField);
                             }
                             if (*side != "BID" && *side != "ASK") {
                                 return std::unexpected(WireError::WrongType);
                             }
                             out.symbol = *symbol;
                             out.price = *price;
                             out.qty = *qty;
                             out.aggressor_bought = *side == "BID";
                             const auto ts = detail::opt_ms(data, "t");
                             if (!ts.has_value()) {
                                 return std::unexpected(ts.error());
                             }
                             out.ts_ms = *ts;
                             return out;
                         });
    });
}

std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame) {
    return guarded([&]() -> std::expected<WireMarkPrice, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [](detail::ondemand::object& data) -> std::expected<WireMarkPrice, WireError> {
                WireMarkPrice out{};
                const auto symbol = detail::req_str(data, "s");
                const auto mark = detail::req_str(data, "p");
                if (!symbol.has_value() || !mark.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.symbol = *symbol;
                out.mark_price = *mark;
                const auto ts = detail::opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                return out;
            });
    });
}

} // namespace raikiri::venues::xt
