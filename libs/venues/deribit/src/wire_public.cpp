#include <array>

#include "wire_detail.hpp"

namespace raikiri::venues::deribit {

namespace {

using detail::guarded;
using detail::session;

// The parameter count distinguishes the two book streams:
// "book.BTC-PERPETUAL.100ms" is incremental, "book.BTC-PERPETUAL.none.1.100ms"
// a depth-limited snapshot. No instrument name contains a dot.
constexpr std::size_t k_max_segments = 6;

struct Channel {
    std::array<std::string_view, k_max_segments> parts{};
    std::size_t count{0};
};

Channel split_channel(std::string_view channel) noexcept {
    Channel out{};
    while (!channel.empty() && out.count < k_max_segments) {
        const auto dot = channel.find('.');
        if (dot == std::string_view::npos) {
            out.parts[out.count++] = channel;
            break;
        }
        out.parts[out.count++] = channel.substr(0, dot);
        channel.remove_prefix(dot + 1);
    }
    return out;
}

EventKind kind_from_channel(const Channel& channel) noexcept {
    if (channel.count < 3) {
        return EventKind::Unknown;
    }
    const auto stream = channel.parts[0];
    if (stream == "trades") {
        return EventKind::Trade;
    }
    if (stream == "ticker") {
        return EventKind::Ticker;
    }
    if (stream == "book") {
        return channel.count >= 5 ? EventKind::Bbo : EventKind::Book;
    }
    if (stream == "user") {
        // user.changes.{kind}.{currency}.raw — the instrument is NOT in the
        // channel here, it is in every row of the payload.
        return channel.parts[1] == "changes" ? EventKind::UserChange : EventKind::Unknown;
    }
    // Other channels exist; only what ws.cpp subscribes to is claimed here.
    return EventKind::Unknown;
}

// True where the channel's SECOND segment is an instrument name. On the
// private channels it is the topic, and copying it into EventInfo would hand
// the mapper "orders" as a symbol to look up.
bool names_instrument(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::Bbo:
    case EventKind::Trade:
    case EventKind::Ticker:
    case EventKind::Book:
        return true;
    case EventKind::UserChange:
    case EventKind::Heartbeat:
    case EventKind::Response:
    case EventKind::Unknown:
        return false;
    }
    return false;
}

// Every implemented push nests its payload one level down from the frame.
std::expected<detail::ondemand::object, WireError> push_data(detail::ondemand::object& root) {
    auto params = detail::req_object(root, "params");
    if (!params.has_value()) {
        return std::unexpected(params.error());
    }
    return detail::req_object(*params, "data");
}

LevelAction action_from(std::string_view text) noexcept {
    if (text == "delete") {
        return LevelAction::Delete;
    }
    return text == "change" ? LevelAction::Change : LevelAction::New;
}

// Grouped books ship [price, amount], the incremental feed [action, price,
// amount]; `expect_action` says which, so a shape change is a refusal rather
// than a price read out of the action slot.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry, bool expect_action) {
    detail::ondemand::array tuple;
    if (const auto ec = entry.get_array().get(tuple); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    const std::size_t want = expect_action ? 3U : 2U;
    std::size_t field = 0;
    for (auto element : tuple) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field >= want) {
            break;
        }
        if (expect_action && field == 0) {
            const auto text = detail::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            level.action = action_from(*text);
        } else {
            const auto token = detail::num_of(value);
            if (!token.has_value()) {
                return std::unexpected(token.error());
            }
            (field == want - 2 ? level.price : level.qty) = *token;
        }
        ++field;
    }
    if (field < want) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

std::expected<std::size_t, WireError> read_levels(detail::ondemand::object& obj,
                                                  std::string_view key,
                                                  bool expect_action,
                                                  std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto level = read_level(value, expect_action);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

// An empty side is a real state on an illiquid option, so it yields empty
// views rather than an error; the mapper drops those.
std::expected<void, WireError> read_top(detail::ondemand::object& obj,
                                        std::string_view key,
                                        std::string_view& price,
                                        std::string_view& qty) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto level = read_level(value, /*expect_action=*/false);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        price = level->price;
        qty = level->qty;
        break;
    }
    return {};
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto method = detail::opt_str(*root, "method");
        if (!method.has_value()) {
            return std::unexpected(method.error());
        }
        EventInfo info{};
        if (method->empty()) {
            info.kind = EventKind::Response; // a reply to something we sent
            return info;
        }
        auto params = detail::req_object(*root, "params");
        if (!params.has_value()) {
            return std::unexpected(params.error());
        }
        if (*method == "heartbeat") {
            const auto type = detail::req_str(*params, "type");
            if (!type.has_value()) {
                return std::unexpected(type.error());
            }
            info.kind = EventKind::Heartbeat;
            info.needs_test_reply = *type == "test_request";
            return info;
        }
        if (*method != "subscription") {
            return info; // Unknown: ignored, never fatal
        }
        const auto channel = detail::req_str(*params, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        const auto parts = split_channel(*channel);
        info.kind = kind_from_channel(parts);
        if (parts.count >= 2 && names_instrument(info.kind)) {
            info.instrument = parts.parts[1];
        }
        return info;
    });
}

std::expected<WireResponse, WireError> parse_response(std::string_view frame) {
    return guarded([&]() -> std::expected<WireResponse, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireResponse out{};
        const auto id = detail::opt_i64(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = *id;

        detail::ondemand::object error;
        const auto error_ec = root->find_field_unordered("error").get_object().get(error);
        if (error_ec == simdjson::SUCCESS) {
            const auto code = detail::opt_i64(error, "code");
            const auto message = detail::opt_str(error, "message");
            if (!code.has_value() || !message.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.error_code = *code;
            out.error_message = *message;
            return out;
        }
        if (error_ec != simdjson::NO_SUCH_FIELD && error_ec != simdjson::INCORRECT_TYPE) {
            return std::unexpected(detail::map_error(error_ec));
        }

        detail::ondemand::array accepted;
        if (root->find_field_unordered("result").get_array().get(accepted) == simdjson::SUCCESS) {
            for (auto entry : accepted) {
                if (const auto ec = entry.error(); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                ++out.accepted;
            }
        }
        return out;
    });
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBbo out{};
        const auto instrument = detail::req_str(*data, "instrument_name");
        if (!instrument.has_value()) {
            return std::unexpected(instrument.error());
        }
        out.instrument = *instrument;
        if (const auto ok = read_top(*data, "bids", out.bid_price, out.bid_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        if (const auto ok = read_top(*data, "asks", out.ask_price, out.ask_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        const auto ts = detail::opt_ms(*data, "timestamp");
        const auto change = detail::opt_i64(*data, "change_id");
        if (!ts.has_value() || !change.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.change_id = *change;
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
        auto params = detail::req_object(*root, "params");
        if (!params.has_value()) {
            return std::unexpected(params.error());
        }
        auto array = detail::req_array(*params, "data");
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
            const auto instrument = detail::req_str(obj, "instrument_name");
            const auto trade_id = detail::opt_str(obj, "trade_id");
            const auto direction = detail::req_str(obj, "direction");
            const auto price = detail::req_num(obj, "price");
            const auto qty = detail::req_num(obj, "amount");
            if (!instrument.has_value() || !trade_id.has_value() || !direction.has_value() ||
                !price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.instrument = *instrument;
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *direction == "buy";
            const auto ts = detail::opt_ms(obj, "timestamp");
            const auto seq = detail::opt_i64(obj, "trade_seq");
            if (!ts.has_value() || !seq.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.ts_ms = *ts;
            out.trade_seq = *seq;
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
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireTicker out{};
        const auto instrument = detail::req_str(*data, "instrument_name");
        const auto mark = detail::req_num(*data, "mark_price");
        if (!instrument.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.instrument = *instrument;
        out.mark_price = *mark;
        // Optional by market, not by chance, and 0.0 on a perpetual is real.
        const auto current = detail::opt_num(*data, "current_funding");
        const auto eight_hour = detail::opt_num(*data, "funding_8h");
        const auto ts = detail::opt_ms(*data, "timestamp");
        if (!current.has_value() || !eight_hour.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.current_funding = *current;
        out.funding_8h = *eight_hour;
        out.ts_ms = *ts;

        // Read before the option scalars so the cursor walks `stats` and then
        // `greeks` in the order the frame writes them. Absent is tolerated:
        // this must never cost the frame its funding rate.
        if (auto stats = detail::req_object(*data, "stats"); stats.has_value()) {
            out.stats_volume = detail::opt_num(*stats, "volume").value_or(std::string_view{});
            out.stats_volume_usd =
                detail::opt_num(*stats, "volume_usd").value_or(std::string_view{});
        }

        // Options only, and every one of these is absent on a perpetual. The
        // scalars are read before the nested object so the cursor never has to
        // climb back out of `greeks` to reach a sibling.
        out.mark_iv = detail::opt_num(*data, "mark_iv").value_or(std::string_view{});
        out.bid_iv = detail::opt_num(*data, "bid_iv").value_or(std::string_view{});
        out.ask_iv = detail::opt_num(*data, "ask_iv").value_or(std::string_view{});
        out.underlying_price =
            detail::opt_num(*data, "underlying_price").value_or(std::string_view{});
        if (auto greeks = detail::req_object(*data, "greeks"); greeks.has_value()) {
            out.delta = detail::opt_num(*greeks, "delta").value_or(std::string_view{});
            out.gamma = detail::opt_num(*greeks, "gamma").value_or(std::string_view{});
            out.vega = detail::opt_num(*greeks, "vega").value_or(std::string_view{});
            out.theta = detail::opt_num(*greeks, "theta").value_or(std::string_view{});
            out.rho = detail::opt_num(*greeks, "rho").value_or(std::string_view{});
        }
        return out;
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBook out{};
        const auto instrument = detail::req_str(*data, "instrument_name");
        const auto type = detail::req_str(*data, "type");
        if (!instrument.has_value() || !type.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.instrument = *instrument;
        out.snapshot = *type == "snapshot";
        const auto bids = read_levels(*data, "bids", /*expect_action=*/true, storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*data, "asks", /*expect_action=*/true, storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto ts = detail::opt_ms(*data, "timestamp");
        const auto change = detail::opt_i64(*data, "change_id");
        // Absent on a snapshot rather than a sentinel: 0 means "chain starts".
        const auto prev = detail::opt_i64(*data, "prev_change_id");
        if (!ts.has_value() || !change.has_value() || !prev.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.change_id = *change;
        out.prev_change_id = *prev;
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

} // namespace raikiri::venues::deribit
