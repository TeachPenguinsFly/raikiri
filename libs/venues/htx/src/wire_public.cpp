#include "wire_detail.hpp"

namespace raikiri::venues::htx {

namespace {

using detail::guarded;
using detail::session;

// The part of a topic after the contract code: "market.BTC-USDT.trade.detail"
// -> "trade.detail". Empty when the shape does not hold.
std::string_view suffix_of(std::string_view topic) {
    const auto first = topic.find('.');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto second = topic.find('.', first + 1);
    if (second == std::string_view::npos) {
        return {};
    }
    return topic.substr(second + 1);
}

EventKind kind_from_topic(std::string_view topic) noexcept {
    const auto suffix = suffix_of(topic);
    if (suffix == "bbo") {
        return EventKind::Bbo;
    }
    if (suffix == "trade.detail") {
        return EventKind::Trade;
    }
    // depth.size_20.high_freq and the aggregated steps carry a different
    // shape; only the one ws.cpp subscribes to is claimed here.
    if (suffix == "depth.step0") {
        return EventKind::Depth;
    }
    if (suffix == "funding_rate") {
        return EventKind::Funding;
    }
    return EventKind::Unknown;
}

// A level and a top-of-book quote are both [price, size], with both members
// written as JSON numbers.
std::expected<void, WireError>
read_pair(detail::ondemand::value entry, std::string_view& price, std::string_view& qty) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto raw = detail::raw_of(value);
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            (field == 0 ? price : qty) = *raw;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<void, WireError> read_quote(detail::ondemand::object& tick,
                                          std::string_view key,
                                          std::string_view& price,
                                          std::string_view& qty) {
    detail::ondemand::value value;
    if (const auto ec = tick.find_field_unordered(key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return read_pair(value, price, qty);
}

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& tick, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(tick, key);
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
        if (const auto ok = read_pair(value, into[count].price, into[count].qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        ++count;
    }
    return count;
}

// Both market-data dialects wrap the payload the same way once the outer
// envelope is off: `tick` on the market socket, `data[0]` on the
// notification socket.
std::expected<detail::ondemand::object, WireError> first_data(detail::ondemand::object& root) {
    auto array = detail::req_array(root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        return obj;
    }
    // An empty data array is how HTX answers a query it could not satisfy;
    // reporting it as a datum would invent a zero-valued one.
    return std::unexpected(WireError::MissingField);
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        EventInfo info{};

        // The notification socket's dialect. Checked first because its frames
        // carry neither `ch` nor `ping`.
        const auto op = detail::opt_str(*root, "op");
        if (!op.has_value()) {
            return std::unexpected(op.error());
        }
        if (!op->empty()) {
            if (*op == "ping") {
                const auto ts = detail::opt_raw(*root, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                info.kind = EventKind::NotifyPing;
                info.keepalive_ts = *ts;
                return info;
            }
            const auto topic = detail::opt_str(*root, "topic");
            if (!topic.has_value()) {
                return std::unexpected(topic.error());
            }
            info.contract_code = detail::code_of(*topic);
            if (*op == "notify") {
                info.kind = kind_from_topic(*topic);
                return info;
            }
            // A sub answer is an acknowledgement or a refusal one field apart,
            // and a refusal read as an acknowledgement looks exactly like a
            // stream that is merely quiet.
            const auto code = detail::opt_raw(*root, "err-code");
            if (!code.has_value()) {
                return std::unexpected(code.error());
            }
            info.kind = (*op == "sub" && *code == "0") ? EventKind::SubscribeAck : EventKind::Error;
            return info;
        }

        const auto ch = detail::opt_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        if (!ch->empty()) {
            info.kind = kind_from_topic(*ch);
            info.contract_code = detail::code_of(*ch);
            return info;
        }

        const auto ping = detail::opt_raw(*root, "ping");
        if (!ping.has_value()) {
            return std::unexpected(ping.error());
        }
        if (!ping->empty()) {
            info.kind = EventKind::MarketPing;
            info.keepalive_ts = *ping;
            return info;
        }

        const auto status = detail::opt_str(*root, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        if (*status == "error") {
            info.kind = EventKind::Error;
            return info;
        }
        if (*status == "ok") {
            info.kind = EventKind::SubscribeAck;
            return info;
        }
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::opt_raw(*root, "err-code");
        const auto msg = detail::opt_str(*root, "err-msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (code->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return StreamError{.code = detail::unquote(*code), .msg = *msg};
    });
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ch = detail::req_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        auto tick = detail::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        WireBbo out{};
        out.contract_code = detail::code_of(*ch);
        if (const auto ok = read_quote(*tick, "bid", out.bid_price, out.bid_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        if (const auto ok = read_quote(*tick, "ask", out.ask_price, out.ask_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        const auto ts = detail::opt_u64(*tick, "ts");
        const auto version = detail::opt_u64(*tick, "version");
        if (!ts.has_value() || !version.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.version = *version;
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
        const auto ch = detail::req_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        const auto code = detail::code_of(*ch);
        auto tick = detail::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        auto array = detail::req_array(*tick, "data");
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
            out.contract_code = code;
            const auto amount = detail::opt_raw(obj, "amount");
            const auto quantity = detail::opt_raw(obj, "quantity");
            const auto ts = detail::opt_u64(obj, "ts");
            const auto id = detail::opt_u64(obj, "id");
            const auto price = detail::opt_raw(obj, "price");
            const auto direction = detail::req_str(obj, "direction");
            if (!amount.has_value() || !quantity.has_value() || !ts.has_value() ||
                !id.has_value() || !price.has_value() || !direction.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (amount->empty() || price->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.amount = *amount;
            out.quantity = *quantity;
            out.price = *price;
            out.trade_id = *id;
            out.ts_ms = *ts;
            out.aggressor_bought = *direction == "buy";
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ch = detail::req_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        auto tick = detail::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        WireDepth out{};
        out.contract_code = detail::code_of(*ch);
        const auto bids = read_levels(*tick, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*tick, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto ts = detail::opt_u64(*tick, "ts");
        const auto version = detail::opt_u64(*tick, "version");
        if (!ts.has_value() || !version.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.version = *version;
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

std::expected<WireFunding, WireError> parse_funding(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFunding, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = detail::opt_u64(*root, "ts");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        auto obj = first_data(*root);
        if (!obj.has_value()) {
            return std::unexpected(obj.error());
        }
        WireFunding out{};
        out.ts_ms = *ts;
        const auto code = detail::req_str(*obj, "contract_code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        out.contract_code = *code;
        const auto funding_ms = detail::opt_ms_str(*obj, "funding_time");
        const auto rate = detail::opt_str(*obj, "funding_rate");
        const auto estimated = detail::opt_str(*obj, "estimated_rate");
        const auto settlement_ms = detail::opt_ms_str(*obj, "settlement_time");
        if (!funding_ms.has_value() || !rate.has_value() || !estimated.has_value() ||
            !settlement_ms.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.funding_ms = *funding_ms;
        out.funding_rate = *rate;
        out.estimated_rate = *estimated;
        out.settlement_ms = *settlement_ms;
        return out;
    });
}

} // namespace raikiri::venues::htx
