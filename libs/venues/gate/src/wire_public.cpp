#include "wire_detail.hpp"

namespace raikiri::venues::gate {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "futures.book_ticker") {
        return EventKind::BookTicker;
    }
    if (channel == "futures.trades") {
        return EventKind::Trade;
    }
    if (channel == "futures.tickers") {
        return EventKind::Ticker;
    }
    // futures.order_book (whole-book snapshots) exists and carries a different
    // shape; only the one ws.cpp subscribes to is claimed here.
    if (channel == "futures.order_book_update") {
        return EventKind::BookUpdate;
    }
    return EventKind::Unknown;
}

// The echoed subscription payload. Its first element is the contract on every
// channel; the depth channel appends an interval and a level count.
std::expected<std::string_view, WireError> first_payload(detail::ondemand::object& root) {
    auto array = detail::req_array(root, "payload");
    if (!array.has_value()) {
        return std::string_view{}; // unsubscribe-all frames carry none
    }
    for (auto entry : *array) {
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        return detail::str_of(value);
    }
    return std::string_view{};
}

std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    const auto price = detail::req_num(obj, "p");
    const auto qty = detail::req_num(obj, "s");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireLevel{.price = *price, .qty = *qty};
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

// Gate signs the trade size instead of naming a side. The magnitude is the
// quantity and the sign is the taker's direction, so both come out of one
// token; the stripped view still points into the caller's buffer.
struct SignedSize {
    std::string_view magnitude;
    bool positive{true};
};

SignedSize split_sign(std::string_view token) noexcept {
    const bool negative = token.starts_with('-');
    // remove_prefix, not substr: substr can throw and this is noexcept.
    if (negative || token.starts_with('+')) {
        token.remove_prefix(1);
    }
    return {.magnitude = token, .positive = !negative};
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = detail::req_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        EventInfo info{};
        // The keepalive answer arrives as its own channel with an EMPTY event
        // string, so the channel has to be consulted before the event.
        if (*channel == "futures.pong") {
            info.kind = EventKind::Pong;
            return info;
        }
        const auto event = detail::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        if (*event == "update") {
            info.kind = kind_from_channel(*channel);
            return info;
        }
        const auto contract = first_payload(*root);
        if (!contract.has_value()) {
            return std::unexpected(contract.error());
        }
        info.contract = *contract;
        // A refusal keeps event:"subscribe" and merely adds an `error` object,
        // so presence of that object is the only thing separating "the venue
        // rejected this channel" from "this channel is quiet".
        detail::ondemand::object error;
        const auto ec = root->find_field_unordered("error").get_object().get(error);
        info.kind = ec == simdjson::SUCCESS ? EventKind::Error : EventKind::SubscribeAck;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto error = detail::req_object(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        std::int64_t code = 0;
        if (const auto ec = error->find_field_unordered("code").get_int64().get(code);
            ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        // Code 4 nests a whole REST-style {"label":...} body inside this text.
        // It is surfaced verbatim for logs; classification works off the code.
        const auto message = detail::opt_str(*error, "message");
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        return StreamError{.code = code, .message = *message};
    });
}

std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBookTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_result_object(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireBookTicker, WireError> {
                WireBookTicker out{};
                const auto ts = detail::opt_u64(obj, "t");
                const auto update_id = detail::opt_u64(obj, "u");
                const auto contract = detail::req_str(obj, "s");
                const auto bid_price = detail::req_num(obj, "b");
                const auto bid_qty = detail::req_num(obj, "B");
                const auto ask_price = detail::req_num(obj, "a");
                const auto ask_qty = detail::req_num(obj, "A");
                if (!ts.has_value() || !update_id.has_value() || !contract.has_value() ||
                    !bid_price.has_value() || !bid_qty.has_value() || !ask_price.has_value() ||
                    !ask_qty.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.ts_ms = *ts;
                out.update_id = *update_id;
                out.contract = *contract;
                out.bid_price = *bid_price;
                out.bid_qty = *bid_qty;
                out.ask_price = *ask_price;
                out.ask_qty = *ask_qty;
                return out;
            });
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "result");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireTrade out{};
            const auto trade_id = detail::opt_u64(obj, "id");
            const auto size = detail::req_num(obj, "size");
            // create_time_ms, not create_time: the latter is whole seconds.
            const auto ts = detail::opt_u64(obj, "create_time_ms");
            const auto price = detail::req_num(obj, "price");
            const auto contract = detail::req_str(obj, "contract");
            if (!trade_id.has_value() || !size.has_value() || !ts.has_value() ||
                !price.has_value() || !contract.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto signed_size = split_sign(*size);
            out.trade_id = static_cast<std::int64_t>(*trade_id);
            out.qty = signed_size.magnitude;
            out.aggressor_bought = signed_size.positive;
            out.ts_ms = *ts;
            out.price = *price;
            out.contract = *contract;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "result");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        for (auto entry : *array) {
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireTicker out{};
            const auto contract = detail::req_str(obj, "contract");
            if (!contract.has_value()) {
                return std::unexpected(contract.error());
            }
            out.contract = *contract;
            const auto mark = detail::opt_num(obj, "mark_price");
            const auto rate = detail::opt_num(obj, "funding_rate");
            const auto indicative = detail::opt_num(obj, "funding_rate_indicative");
            const auto interval = detail::opt_u64(obj, "funding_interval");
            const auto next_apply = detail::opt_u64(obj, "funding_next_apply");
            const auto index = detail::opt_num(obj, "index_price");
            const auto ts = detail::opt_u64(obj, "t");
            if (!mark.has_value() || !rate.has_value() || !indicative.has_value() ||
                !interval.has_value() || !next_apply.has_value() || !index.has_value() ||
                !ts.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.mark_price = *mark;
            out.funding_rate = *rate;
            out.funding_rate_indicative = *indicative;
            out.funding_interval_s = static_cast<std::uint32_t>(*interval);
            out.funding_next_apply_s = *next_apply;
            out.index_price = *index;
            out.ts_ms = *ts;
            return out;
        }
        // An empty result array carries no ticker; reporting it as one would
        // invent a zero-valued funding rate.
        return std::unexpected(WireError::MissingField);
    });
}

std::expected<WireBookUpdate, WireError> parse_book_update(std::string_view frame,
                                                           LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBookUpdate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_result_object(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBookUpdate, WireError> {
                WireBookUpdate out{};
                const auto ts = detail::opt_u64(obj, "t");
                const auto first = detail::opt_u64(obj, "U");
                const auto last = detail::opt_u64(obj, "u");
                const auto contract = detail::req_str(obj, "s");
                if (!ts.has_value() || !first.has_value() || !last.has_value() ||
                    !contract.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.ts_ms = *ts;
                out.first_update_id = *first;
                out.last_update_id = *last;
                out.contract = *contract;
                // `a` before `b`: the frame lists asks first and reading the
                // object in wire order avoids a rewind per message.
                const auto asks = read_levels(obj, "a", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto bids = read_levels(obj, "b", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                out.asks = {storage.asks.data(), *asks};
                out.bids = {storage.bids.data(), *bids};
                return out;
            });
    });
}

} // namespace raikiri::venues::gate
