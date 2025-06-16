#include "wire_detail.hpp"

namespace raikiri::venues::bingx {

namespace {

using detail::guarded;
using detail::session;

// Exactly these four bytes after inflation; the answer is "Pong", also bare.
constexpr std::string_view k_ping = "Ping";

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "bookTicker") {
        return EventKind::BookTicker;
    }
    if (channel == "trade") {
        return EventKind::Trade;
    }
    if (channel == "markPrice") {
        return EventKind::MarkPrice;
    }
    if (channel.starts_with("depth")) {
        return EventKind::Depth;
    }
    return EventKind::Unknown;
}

// A level is ["64148.0","7.4404"]: price then size, both strings.
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

} // namespace

bool is_ping(std::string_view frame) noexcept {
    return detail::trim(frame) == k_ping;
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    if (is_ping(frame)) {
        return EventInfo{.kind = EventKind::Ping, .symbol = {}};
    }
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto data_type = detail::opt_str(*root, "dataType");
        if (!data_type.has_value()) {
            return std::unexpected(data_type.error());
        }
        EventInfo info{};
        if (data_type->empty()) {
            // Its code is the only thing separating an ack from a refusal.
            const auto code = detail::req_i64(*root, "code");
            if (!code.has_value()) {
                return std::unexpected(code.error());
            }
            info.kind = *code == k_ok ? EventKind::ControlAck : EventKind::Error;
            return info;
        }
        info.symbol = detail::symbol_of(*data_type);
        info.kind = kind_from_channel(data_type->substr(info.symbol.size() + 1));
        return info;
    });
}

std::expected<ControlFrame, WireError> parse_control(std::string_view frame) {
    return guarded([&]() -> std::expected<ControlFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto id = detail::opt_str(*root, "id");
        const auto code = detail::opt_i64(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        if (!id.has_value() || !code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return ControlFrame{.id = *id, .code = *code, .msg = *msg};
    });
}

std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBookTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBookTicker out{};
        const auto symbol = detail::req_str(*data, "s");
        const auto bid = detail::req_str(*data, "b");
        const auto bid_qty = detail::req_str(*data, "B");
        const auto ask = detail::req_str(*data, "a");
        const auto ask_qty = detail::req_str(*data, "A");
        if (!symbol.has_value() || !bid.has_value() || !bid_qty.has_value() || !ask.has_value() ||
            !ask_qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.bid_price = *bid;
        out.bid_qty = *bid_qty;
        out.ask_price = *ask;
        out.ask_qty = *ask_qty;
        // opt_* answers 0 for a field that is not there, which is what makes
        // this frame readable on both trees: the swap carries u and T, spot
        // carries neither, and only E is common. A zero here means ABSENT and
        // must not be read as a sequence number or a timestamp.
        const auto update_id = detail::opt_u64(*data, "u");
        const auto transaction = detail::opt_u64(*data, "T");
        const auto event = detail::opt_u64(*data, "E");
        if (!update_id.has_value() || !transaction.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.update_id = *update_id;
        out.transaction_ms = *transaction;
        out.event_ms = *event;
        return out;
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        const auto read_one = [&](detail::ondemand::object& obj)
            -> std::expected<void, WireError> {
            WireTrade out{};
            const auto symbol = detail::req_str(obj, "s");
            const auto price = detail::req_str(obj, "p");
            const auto qty = detail::req_str(obj, "q");
            if (!symbol.has_value() || !price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.price = *price;
            out.qty = *qty;
            const auto trade_ms = detail::opt_u64(obj, "T");
            const auto maker = detail::opt_bool(obj, "m");
            if (!trade_ms.has_value() || !maker.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.trade_ms = *trade_ms;
            out.buyer_is_maker = *maker;
            storage.trades[count] = out;
            ++count;
            return {};
        };

        // The swap tree sends an array of prints under "data"; the spot tree
        // sends ONE object under the same key. Same envelope, same field
        // names, different arity — so the shape is read off the raw text and
        // then decoded once. Asking for an array and falling back on failure
        // does not work: a refused type conversion leaves the On-Demand
        // document somewhere a rewind cannot recover.
        const auto raw = detail::opt_raw(*root, "data");
        if (!raw.has_value()) {
            return std::unexpected(raw.error());
        }
        const auto text = detail::trim(*raw);
        root->reset();
        if (text.starts_with('[')) {
            auto array = detail::req_array(*root, "data");
            if (!array.has_value()) {
                return std::unexpected(array.error());
            }
            for (auto entry : *array) {
                if (count == k_max_trades) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                detail::ondemand::object obj;
                if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                if (const auto one = read_one(obj); !one.has_value()) {
                    return std::unexpected(one.error());
                }
            }
            return std::span<const WireTrade>{storage.trades.data(), count};
        }
        auto single = detail::req_object(*root, "data");
        if (!single.has_value()) {
            return std::unexpected(single.error());
        }
        if (const auto one = read_one(*single); !one.has_value()) {
            return std::unexpected(one.error());
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame) {
    return guarded([&]() -> std::expected<WireMarkPrice, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireMarkPrice out{};
        const auto symbol = detail::req_str(*data, "s");
        const auto mark = detail::req_str(*data, "p");
        if (!symbol.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.mark_price = *mark;
        const auto event = detail::opt_u64(*data, "E");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        out.event_ms = *event;
        return out;
    });
}

std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto data_type = detail::req_str(*root, "dataType");
        // The swap envelope stamps "ts"; the spot one stamps "timestamp".
        const auto ts = detail::opt_u64(*root, "ts");
        const auto stamp = detail::opt_u64(*root, "timestamp");
        if (!data_type.has_value() || !ts.has_value() || !stamp.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto bids = read_levels(*data, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*data, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        WireDepth out{};
        out.symbol = detail::symbol_of(*data_type);
        if (out.symbol.empty()) {
            return std::unexpected(WireError::MissingField); // nothing else names it
        }
        out.ts_ms = *ts != 0 ? *ts : *stamp;
        // SPOT SENDS ASKS WORST FIRST. Both trees send bids best-first, so the
        // asymmetry reads as a normal book right up to the point where the top
        // of it is a hundred dollars away from the bid — and crossed against
        // it, which is a signal every strategy acts on.
        if (market == MarketType::Spot) {
            std::reverse(storage.asks.begin(), storage.asks.begin() + static_cast<long>(*asks));
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

} // namespace raikiri::venues::bingx
