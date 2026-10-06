#include <raikiri/venues/bitstamp/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitstamp {

namespace {

std::string_view text(wire::ondemand::object& obj, std::string_view key) {
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

std::uint64_t quoted_u64(wire::ondemand::object& obj, std::string_view key) {
    return wire::digits_to_u64(text(obj, key));
}

template <typename Row, std::size_t N, typename Fill>
std::expected<std::span<const Row>, WireError> read_rows(std::string_view body, std::array<Row, N>& rows, Fill&& fill) {
    return wire::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto array = wire::session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            Row row{};
            fill(obj, row);
            rows[count] = row;
            ++count;
        }
        return std::span<const Row>{rows.data(), count};
    });
}

std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& data, std::string_view key, std::span<WireLevel> into) {
    auto array = wire::req_array(data, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        WireLevel level{};
        std::size_t field = 0;
        for (auto element : pair) {
            wire::ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto token = wire::str_of(value);
            if (!token.has_value()) {
                return std::unexpected(token.error());
            }
            (field == 0 ? level.price : level.qty) = *token;
            if (++field == 2) {
                break;
            }
        }
        if (field < 2) {
            return std::unexpected(WireError::ShortArray);
        }
        into[count] = level;
        ++count;
    }
    return count;
}

template <typename Out, typename Fill> std::expected<Out, WireError> read_data(std::string_view frame, Fill&& fill) {
    return wire::guarded([&]() -> std::expected<Out, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        return fill(*data);
    });
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return read_rows(body, storage.rows, [](wire::ondemand::object& obj, WireMarket& row) {
        row.market_symbol = text(obj, "market_symbol");
        row.base_currency = text(obj, "base_currency");
        row.counter_currency = text(obj, "counter_currency");
        row.market_type = text(obj, "market_type");
        row.asset_class = text(obj, "asset_class");
        row.trading = text(obj, "trading");
        row.minimum_order_value = text(obj, "minimum_order_value");
        row.contract_size = text(obj, "contract_size");
        row.base_decimals = static_cast<std::uint32_t>(wire::opt_u64(obj, "base_decimals").value_or(0));
        row.counter_decimals = static_cast<std::uint32_t>(wire::opt_u64(obj, "counter_decimals").value_or(0));
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body, TickerBuffer& storage) {
    return read_rows(body, storage.rows, [](wire::ondemand::object& obj, WireTicker& row) {
        row.pair = text(obj, "pair");
        row.volume = text(obj, "volume");
        row.vwap = text(obj, "vwap");
        row.open_interest = text(obj, "open_interest");
        row.ts_s = quoted_u64(obj, "timestamp");
    });
}

Head parse_head(std::string_view frame) {
    const auto head = wire::guarded([&]() -> std::expected<Head, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        Head out{};
        const auto event = text(*root, "event");
        out.channel = text(*root, "channel");
        if (event == "data") {
            out.event = Event::Book;
        } else if (event == "trade") {
            out.event = Event::Trade;
        } else if (event == "funding_rate_saved") {
            out.event = Event::Funding;
        } else if (event == "bts:subscription_succeeded") {
            out.event = Event::Subscribed;
        } else if (event == "bts:error") {
            out.event = Event::Refused;
        } else if (event == "bts:request_reconnect") {
            out.event = Event::Reconnect;
        }
        return out;
    });
    return head.value_or(Head{});
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return read_data<WireBook>(frame, [&](wire::ondemand::object& data) -> std::expected<WireBook, WireError> {
        WireBook out{};
        out.ts_us = quoted_u64(data, "microtimestamp");
        const auto bids = read_levels(data, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(data, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        out.bids = std::span<const WireLevel>{storage.bids.data(), *bids};
        out.asks = std::span<const WireLevel>{storage.asks.data(), *asks};
        return out;
    });
}

std::expected<WireTrade, WireError> parse_trade(std::string_view frame) {
    return read_data<WireTrade>(frame, [](wire::ondemand::object& data) -> std::expected<WireTrade, WireError> {
        WireTrade out{};
        out.id = wire::opt_u64(data, "id").value_or(0);
        out.price = text(data, "price_str");
        out.qty = text(data, "amount_str");
        const auto type = wire::opt_u64(data, "type");
        if (!type.has_value() || *type > 1) {
            return std::unexpected(WireError::MissingField);
        }
        out.aggressor_bought = *type == 0;
        out.ts_us = quoted_u64(data, "microtimestamp");
        return out;
    });
}

std::expected<WireFunding, WireError> parse_funding(std::string_view frame) {
    return read_data<WireFunding>(frame, [](wire::ondemand::object& data) -> std::expected<WireFunding, WireError> {
        WireFunding out{};
        out.market = text(data, "market");
        out.rate = text(data, "funding_rate");
        out.mark = text(data, "mark_price");
        out.index = text(data, "index_price");
        out.ts_s = quoted_u64(data, "timestamp");
        out.next_s = quoted_u64(data, "next_funding_time");
        if (out.rate.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

} // namespace raikiri::venues::bitstamp
