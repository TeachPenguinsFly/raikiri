#include <raikiri/venues/grvt/endpoints.hpp>
#include <raikiri/venues/grvt/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::grvt {

namespace json = raikiri::wire;

namespace {

namespace ondemand = simdjson::ondemand;

using json::guarded;
using json::session;

// Absent and null are different facts on a delta stream: absent leaves the
// field as it was, null clears it.
std::expected<std::optional<std::string_view>, WireError> present_str(ondemand::object& obj,
                                                                      std::string_view key) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::optional<std::string_view>{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(json::map_error(ec));
    }
    const auto text = json::str_of(value);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return std::optional<std::string_view>{*text};
}

std::expected<std::string_view, WireError>
string_of(simdjson::simdjson_result<ondemand::value> entry) {
    ondemand::value value;
    if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(json::map_error(ec));
    }
    return json::str_of(value);
}

std::expected<std::uint64_t, WireError> digits(ondemand::object& obj, std::string_view key) {
    const auto text = json::opt_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return json::digits_to_u64(*text);
}

std::expected<std::span<const WireLevel>, WireError> parse_levels(
    ondemand::object& feed, std::string_view key, std::array<WireLevel, k_max_levels>& out) {
    auto levels = json::req_array(feed, key);
    if (!levels.has_value()) {
        return std::unexpected(levels.error());
    }
    std::size_t count = 0;
    for (auto entry : *levels) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::object level;
        if (const auto ec = entry.get_object().get(level); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        const auto price = json::req_str(level, "price");
        const auto size = json::req_str(level, "size");
        if (!price.has_value() || !size.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out[count] = WireLevel{.price = *price, .size = *size};
        ++count;
    }
    return std::span<const WireLevel>{out.data(), count};
}

std::expected<void, WireError> parse_quote(ondemand::object& feed, WireQuote& out) {
    const std::array<std::pair<std::string_view, std::optional<std::string_view>*>, 4> fields{{
        {"best_bid_price", &out.bid_price},
        {"best_bid_size", &out.bid_size},
        {"best_ask_price", &out.ask_price},
        {"best_ask_size", &out.ask_size},
    }};
    for (const auto& [key, into] : fields) {
        const auto value = present_str(feed, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    return {};
}

std::expected<void, WireError> parse_print(ondemand::object& feed, WirePrint& out) {
    const auto price = json::req_str(feed, "price");
    const auto size = json::req_str(feed, "size");
    const auto taker = json::opt_bool(feed, "is_taker_buyer");
    if (!price.has_value() || !size.has_value() || !taker.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.price = *price;
    out.size = *size;
    out.taker_bought = *taker;
    return {};
}

std::expected<void, WireError> parse_ticker(ondemand::object& feed, WireTicker& out) {
    const std::array<std::pair<std::string_view, std::string_view*>, 6> fields{{
        {"mark_price", &out.mark_price},
        {"funding_rate", &out.funding_rate},
        {"buy_volume_24h_b", &out.buy_volume_base},
        {"sell_volume_24h_b", &out.sell_volume_base},
        {"buy_volume_24h_q", &out.buy_volume_quote},
        {"sell_volume_24h_q", &out.sell_volume_quote},
    }};
    for (const auto& [key, into] : fields) {
        const auto value = json::opt_str(feed, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    const auto next = digits(feed, "next_funding_time");
    const auto hours = json::opt_u64(feed, "funding_interval_hours");
    if (!next.has_value() || !hours.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.next_funding_ns = *next;
    out.funding_interval_hours = static_cast<std::uint32_t>(*hours);
    return {};
}

FeedKind kind_of(std::string_view stream) noexcept {
    if (stream == ep::k_quote_stream) {
        return FeedKind::Quote;
    }
    if (stream == ep::k_book_stream) {
        return FeedKind::Book;
    }
    if (stream == ep::k_trade_stream) {
        return FeedKind::Trade;
    }
    if (stream == ep::k_ticker_stream) {
        return FeedKind::Ticker;
    }
    return FeedKind::Unknown;
}

std::expected<WireMessage, WireError>
parse_feed(ondemand::object& root, std::string_view stream, MessageBuffer& storage) {
    WireMessage out{};
    auto& feed = out.feed;
    feed.stream = stream;
    feed.kind = kind_of(stream);
    const auto selector = json::req_str(root, "selector");
    const auto sequence = digits(root, "sequence_number");
    if (!selector.has_value() || !sequence.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    feed.selector = *selector;
    feed.sequence = *sequence;
    if (feed.kind == FeedKind::Unknown) {
        return out;
    }
    auto body = json::req_object(root, "feed");
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    const auto event = digits(*body, "event_time");
    if (!event.has_value()) {
        return std::unexpected(event.error());
    }
    feed.event_ns = *event;
    std::expected<void, WireError> parsed{};
    switch (feed.kind) {
    case FeedKind::Quote:
        parsed = parse_quote(*body, feed.quote);
        break;
    case FeedKind::Trade:
        parsed = parse_print(*body, feed.print);
        break;
    case FeedKind::Ticker:
        parsed = parse_ticker(*body, feed.ticker);
        break;
    case FeedKind::Book: {
        const auto bids = parse_levels(*body, "bids", storage.levels.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = parse_levels(*body, "asks", storage.levels.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        feed.book = WireBook{.bids = *bids, .asks = *asks};
        break;
    }
    case FeedKind::Unknown:
        break;
    }
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    const auto prev = digits(root, "prev_sequence_number");
    if (!prev.has_value()) {
        return std::unexpected(prev.error());
    }
    feed.prev_sequence = *prev;
    return out;
}

std::expected<WireMessage, WireError> parse_control(ondemand::object& root,
                                                    MessageBuffer& storage) {
    WireMessage out{};
    if (json::has_field(root, "error")) {
        auto error = json::req_object(root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        const auto code = json::opt_i64(*error, "code");
        const auto message = json::opt_str(*error, "message");
        if (!code.has_value() || !message.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.kind = MessageKind::Error;
        out.error = WireRpcError{.code = *code, .message = *message};
        return out;
    }
    auto result = json::req_object(root, "result");
    if (!result.has_value()) {
        return std::unexpected(WireError::UnknownMessage);
    }
    const auto stream = json::req_str(*result, "stream");
    if (!stream.has_value()) {
        return std::unexpected(stream.error());
    }
    auto subs = json::req_array(*result, "subs");
    if (!subs.has_value()) {
        return std::unexpected(subs.error());
    }
    std::size_t sub_count = 0;
    for (auto entry : *subs) {
        if (sub_count == k_max_acked) {
            return std::unexpected(WireError::TooManyEntries);
        }
        const auto text = string_of(entry);
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        storage.ack.subs[sub_count++] = *text;
    }
    auto latest = json::req_array(*result, "latest_sequence_number");
    if (!latest.has_value()) {
        return std::unexpected(latest.error());
    }
    std::size_t latest_count = 0;
    for (auto entry : *latest) {
        if (latest_count == k_max_acked) {
            return std::unexpected(WireError::TooManyEntries);
        }
        const auto text = string_of(entry);
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        storage.ack.latest[latest_count++] = json::digits_to_u64(*text);
    }
    // Two arrays read in the same order; a length mismatch would pair a
    // selector with another selector's sequence.
    if (latest_count != sub_count) {
        return std::unexpected(WireError::ShortArray);
    }
    out.kind = MessageKind::Ack;
    out.ack =
        WireAck{.stream = *stream,
                .subs = std::span<const std::string_view>{storage.ack.subs.data(), sub_count},
                .latest = std::span<const std::uint64_t>{storage.ack.latest.data(), latest_count}};
    return out;
}

} // namespace

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "result");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            WireInstrument out{};
            const auto name = json::req_str(obj, "instrument");
            if (!name.has_value()) {
                return std::unexpected(name.error());
            }
            out.instrument = *name;
            const std::array<std::pair<std::string_view, std::string_view*>, 9> fields{{
                {"base", &out.base},
                {"quote", &out.quote},
                {"kind", &out.kind},
                {"status", &out.status},
                {"tick_size", &out.tick_size},
                {"min_size", &out.min_size},
                {"min_notional", &out.min_notional},
                {"adjusted_funding_rate_cap", &out.funding_cap},
                {"adjusted_funding_rate_floor", &out.funding_floor},
            }};
            for (const auto& [key, into] : fields) {
                const auto value = json::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *into = *value;
            }
            const auto hours = json::opt_u64(obj, "funding_interval_hours");
            if (!hours.has_value()) {
                return std::unexpected(hours.error());
            }
            out.funding_interval_hours = static_cast<std::uint32_t>(*hours);
            storage.rows[count++] = out;
        }
        return std::span<const WireInstrument>{storage.rows.data(), count};
    });
}

std::optional<WireRestError> parse_rest_error(std::string_view body) {
    const auto parsed = guarded([&]() -> std::expected<WireRestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (!json::has_field(*root, "code")) {
            return std::unexpected(WireError::MissingField);
        }
        const auto code = json::opt_i64(*root, "code");
        const auto message = json::opt_str(*root, "message");
        if (!code.has_value() || !message.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return WireRestError{.code = *code, .message = *message};
    });
    return parsed.has_value() ? std::optional{*parsed} : std::nullopt;
}

std::expected<WireMessage, WireError> parse_message(std::string_view frame,
                                                    MessageBuffer& storage) {
    return guarded([&]() -> std::expected<WireMessage, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // A feed frame opens with its stream; a JSON-RPC answer has none at the
        // top level, only inside `result`.
        const auto stream = json::opt_str(*root, "stream");
        if (!stream.has_value()) {
            return std::unexpected(stream.error());
        }
        if (!stream->empty()) {
            return parse_feed(*root, *stream, storage);
        }
        return parse_control(*root, storage);
    });
}

std::string_view selector_instrument(std::string_view selector) noexcept {
    return selector.substr(0, selector.find('@'));
}

} // namespace raikiri::venues::grvt
