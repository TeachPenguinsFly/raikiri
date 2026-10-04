#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/spot.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <utility>

namespace raikiri::venues::kraken {

namespace {

using wire::guarded;
using wire::session;

struct Alias {
    std::string_view v1;
    std::string_view v2;
};

constexpr std::array<Alias, 2> k_v2_aliases{{{"XBT", "BTC"}, {"XDG", "DOGE"}}};

std::string_view v2_asset(std::string_view asset) noexcept {
    for (const auto& alias : k_v2_aliases) {
        if (asset == alias.v1) {
            return alias.v2;
        }
    }
    return asset;
}

std::expected<std::uint64_t, WireError> read_timestamp(wire::ondemand::object& obj) {
    const auto text = wire::req_str(obj, "timestamp");
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return parse_iso8601_ns(*text);
}

// the first entry of `data`; every channel this adapter reads sends one symbol per message
template <typename Fn>
auto first_entry(std::string_view frame, Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto root = session().open_object(frame);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto data = wire::req_array(*root, "data");
    if (!data.has_value()) {
        return std::unexpected(data.error());
    }
    for (auto entry : *data) {
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return fn(obj);
    }
    return std::unexpected(WireError::MissingField);
}

} // namespace

std::expected<std::span<const WireSpotPair>, WireError> parse_asset_pairs(std::string_view body,
                                                                          SpotPairBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotPair>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto errors = wire::req_array(*root, "error");
        if (!errors.has_value()) {
            return std::unexpected(errors.error());
        }
        for (auto ignored : *errors) {
            (void)ignored;
            return std::unexpected(WireError::UnknownMessage);
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        std::size_t count = 0;
        const auto walked = wire::for_each_field(
            *result, [&](std::string_view, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (count == k_max_spot_pairs) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                wire::ondemand::object obj;
                if (const auto ec = value.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                WireSpotPair out{};
                const auto altname = wire::req_str(obj, "altname");
                const auto wsname = wire::opt_str(obj, "wsname");
                const auto status = wire::opt_str(obj, "status");
                const auto ordermin = wire::opt_str(obj, "ordermin");
                const auto costmin = wire::opt_str(obj, "costmin");
                if (!altname.has_value() || !wsname.has_value() || !status.has_value() || !ordermin.has_value() ||
                    !costmin.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                bool has_price = false;
                bool has_lot = false;
                const auto price = opt_i64(obj, "pair_decimals", has_price);
                const auto lot = opt_i64(obj, "lot_decimals", has_lot);
                if (!price.has_value() || !lot.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.altname = *altname;
                out.wsname = *wsname;
                out.status = *status;
                out.ordermin = *ordermin;
                out.costmin = *costmin;
                out.pair_decimals = has_price ? *price : -1;
                out.lot_decimals = has_lot ? *lot : -1;
                storage.pairs[count] = out;
                ++count;
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        return std::span<const WireSpotPair>{storage.pairs.data(), count};
    });
}

std::optional<SymbolText> v2_symbol(std::string_view wsname) noexcept {
    const auto slash = wsname.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == wsname.size() ||
        wsname.find('/', slash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto base = v2_asset(wsname.substr(0, slash));
    const auto quote = v2_asset(wsname.substr(slash + 1));
    std::array<char, 64> buf{};
    if (base.size() + 1 + quote.size() > buf.size()) {
        return std::nullopt;
    }
    std::size_t len = base.copy(buf.data(), base.size());
    buf[len++] = '/';
    len += quote.copy(buf.data() + len, quote.size());
    const auto symbol = SymbolText::from(std::string_view{buf.data(), len});
    return symbol.has_value() ? std::optional{*symbol} : std::nullopt;
}

std::expected<SpotEvent, WireError> parse_spot_event(std::string_view frame) {
    return guarded([&]() -> std::expected<SpotEvent, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (wire::has_field(*root, "method")) {
            const auto success = wire::opt_bool(*root, "success");
            if (!success.has_value()) {
                return std::unexpected(success.error());
            }
            return SpotEvent{.channel = *success ? SpotChannel::Ack : SpotChannel::Refused};
        }
        const auto channel = wire::opt_str(*root, "channel");
        const auto type = wire::opt_str(*root, "type");
        if (!channel.has_value() || !type.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        SpotEvent out{.snapshot = *type == "snapshot"};
        if (*channel == "ticker") {
            out.channel = SpotChannel::Ticker;
        } else if (*channel == "trade") {
            out.channel = SpotChannel::Trade;
        } else if (*channel == "book") {
            out.channel = SpotChannel::Book;
        }
        return out;
    });
}

std::expected<WireSpotTicker, WireError> parse_spot_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireSpotTicker, WireError> {
        return first_entry(frame, [](wire::ondemand::object& obj) -> std::expected<WireSpotTicker, WireError> {
            WireSpotTicker out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            for (const auto& [key, field] : {std::pair{"bid", &out.bid},
                                             std::pair{"bid_qty", &out.bid_qty},
                                             std::pair{"ask", &out.ask},
                                             std::pair{"ask_qty", &out.ask_qty},
                                             std::pair{"volume", &out.volume},
                                             std::pair{"vwap", &out.vwap}}) {
                const auto value = opt_num(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto ts = read_timestamp(obj);
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            out.ts_ns = *ts;
            return out;
        });
    });
}

std::expected<std::span<const WireSpotTrade>, WireError> parse_spot_trades(std::string_view frame,
                                                                           SpotTradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_array(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        std::size_t count = 0;
        for (auto entry : *data) {
            if (count == k_max_spot_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireSpotTrade out{};
            const auto symbol = wire::req_str(obj, "symbol");
            const auto side = wire::req_str(obj, "side");
            const auto price = req_num(obj, "price");
            const auto qty = req_num(obj, "qty");
            const auto id = opt_num(obj, "trade_id");
            if (!symbol.has_value() || !side.has_value() || !price.has_value() || !qty.has_value() || !id.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto ts = read_timestamp(obj);
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            out.symbol = *symbol;
            out.aggressor_bought = *side == "buy";
            out.price = *price;
            out.qty = *qty;
            out.trade_id = wire::digits_to_u64(*id);
            out.ts_ns = *ts;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireSpotTrade>{storage.trades.data(), count};
    });
}

std::string_view spot_channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "ticker";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        return "book";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::string spot_frame(std::string_view method, std::string_view channel, std::span<const std::string_view> symbols) {
    std::string out;
    out.reserve(96 + symbols.size() * 16);
    out.append(R"({"method":")").append(method).append(R"(","params":{"channel":")").append(channel);
    out.append(R"(","symbol":[)");
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        out.append(i == 0 ? "\"" : ",\"").append(symbols[i]).append("\"");
    }
    out.append("]");
    // the ticker fires on every touch change, not only on trades
    if (channel == "ticker" && method == "subscribe") {
        out.append(R"(,"event_trigger":"bbo")");
    }
    static_assert(k_spot_book_depth == 10);
    if (channel == "book") {
        out.append(R"(,"depth":10)");
    }
    out.append("}}");
    return out;
}

} // namespace raikiri::venues::kraken
