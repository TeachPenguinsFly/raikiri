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
    if (channel == "futures.order_book_update") {
        return EventKind::BookUpdate;
    }
    if (channel == "spot.book_ticker") {
        return EventKind::BookTicker;
    }
    if (channel == "spot.trades") {
        return EventKind::Trade;
    }
    if (channel == "spot.order_book") {
        return EventKind::BookSnapshot;
    }
    if (channel == "futures.orders" || channel == "spot.orders") {
        return EventKind::OrderUpdate;
    }
    if (channel == "futures.usertrades" || channel == "spot.usertrades") {
        return EventKind::UserTrade;
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

namespace {

std::expected<WireTrade, WireError> read_futures_trade(detail::ondemand::object& obj) {
    WireTrade out{};
    const auto trade_id = detail::opt_u64(obj, "id");
    const auto size = detail::req_num(obj, "size");
    // create_time_ms, not create_time: the latter is whole seconds.
    const auto ts = detail::opt_u64(obj, "create_time_ms");
    const auto price = detail::req_num(obj, "price");
    const auto contract = detail::req_str(obj, "contract");
    if (!trade_id.has_value() || !size.has_value() || !ts.has_value() || !price.has_value() ||
        !contract.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    // The SIGN of the size is the side; there is no side field.
    const auto signed_size = split_sign(*size);
    out.trade_id = static_cast<std::int64_t>(*trade_id);
    out.qty = signed_size.magnitude;
    out.aggressor_bought = signed_size.positive;
    out.ts_ms = *ts;
    out.price = *price;
    out.contract = *contract;
    return out;
}

// Spot renames every field the futures print uses and says the side outright:
// `amount` for size, `currency_pair` for the symbol, and `side` naming the
// TAKER — so no sign to split. Its create_time_ms is a decimal STRING with a
// fractional part ("1787832783276.263"), which whole_u64 truncates at the dot.
std::expected<WireTrade, WireError> read_spot_trade(detail::ondemand::object& obj) {
    WireTrade out{};
    const auto trade_id = detail::opt_u64(obj, "id");
    const auto amount = detail::req_num(obj, "amount");
    const auto ts = detail::req_num(obj, "create_time_ms");
    const auto price = detail::req_num(obj, "price");
    const auto pair = detail::req_str(obj, "currency_pair");
    const auto side = detail::req_str(obj, "side");
    if (!trade_id.has_value() || !amount.has_value() || !ts.has_value() || !price.has_value() ||
        !pair.has_value() || !side.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    if (*side != "buy" && *side != "sell") {
        return std::unexpected(WireError::WrongType);
    }
    out.trade_id = static_cast<std::int64_t>(*trade_id);
    out.qty = *amount;
    out.aggressor_bought = *side == "buy";
    out.ts_ms = detail::whole_u64(*ts);
    out.price = *price;
    out.contract = *pair;
    return out;
}

} // namespace

std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage, MarketType market) {
    const bool spot = market == MarketType::Spot;
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // Futures batch prints into an array; spot sends one object.
        if (spot) {
            auto obj = detail::req_object(*root, "result");
            if (!obj.has_value()) {
                return std::unexpected(obj.error());
            }
            const auto one = read_spot_trade(*obj);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[0] = *one;
            return std::span<const WireTrade>{storage.trades.data(), 1};
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
            const auto one = read_futures_trade(obj);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[count] = *one;
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
            // OPTIONAL, unlike the rest: a contract that has not traded in the
            // window carries no volume field at all, and refusing the whole
            // frame over it would drop its funding rate too.
            out.volume_24h_base =
                detail::opt_num(obj, "volume_24h_base").value_or(std::string_view{});
            out.volume_24h_quote =
                detail::opt_num(obj, "volume_24h_quote").value_or(std::string_view{});
            out.ts_ms = *ts;
            return out;
        }
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
                const auto asks = detail::read_levels(obj, "a", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto bids = detail::read_levels(obj, "b", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                out.asks = {storage.asks.data(), *asks};
                out.bids = {storage.bids.data(), *bids};
                return out;
            });
    });
}

std::expected<WireBookUpdate, WireError> parse_book_snapshot(std::string_view frame,
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
                const auto last = detail::opt_u64(obj, "lastUpdateId");
                const auto contract = detail::req_str(obj, "s");
                if (!ts.has_value() || !last.has_value() || !contract.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.ts_ms = *ts;
                // Both ids are the same number on purpose: a snapshot IS its
                // own first and last update, and there is no chain to join.
                out.first_update_id = *last;
                out.last_update_id = *last;
                out.contract = *contract;
                const auto asks = detail::read_pair_levels(obj, "asks", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto bids = detail::read_pair_levels(obj, "bids", storage.bids);
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
