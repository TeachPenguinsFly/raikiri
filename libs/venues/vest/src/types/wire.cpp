#include <raikiri/venues/vest/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::vest {

namespace json = raikiri::wire;

namespace {

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
read_rows(json::ondemand::array& rows, std::span<Row> into, Read&& read) {
    std::size_t count = 0;
    for (auto entry : rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        const auto one = read(row);
        if (!one.has_value()) {
            return std::unexpected(one.error());
        }
        into[count] = *one;
        ++count;
    }
    return std::span<const Row>{into.data(), count};
}

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
rows_under(std::string_view body, std::string_view key, std::span<Row> into, Read&& read) {
    return json::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, key);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, into, read);
    });
}

std::expected<WireMarket, WireError> read_market(json::ondemand::object& row) {
    const auto symbol = json::req_str(row, "symbol");
    const auto base = json::req_str(row, "base");
    const auto quote = json::req_str(row, "quote");
    const auto size = json::opt_u64(row, "sizeDecimals");
    const auto price = json::opt_u64(row, "priceDecimals");
    const auto fee = json::opt_str(row, "takerFee");
    const auto asset = json::req_str(row, "asset");
    const auto status = json::req_str(row, "tradingStatus");
    if (!symbol.has_value() || !base.has_value() || !quote.has_value() || !size.has_value() || !price.has_value() ||
        !fee.has_value() || !asset.has_value() || !status.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireMarket{.symbol = *symbol,
                      .base = *base,
                      .quote = *quote,
                      .asset = *asset,
                      .status = *status,
                      .taker_fee = *fee,
                      .price_decimals = static_cast<std::uint32_t>(*price),
                      .size_decimals = static_cast<std::uint32_t>(*size)};
}

std::expected<std::size_t, WireError> read_side(json::ondemand::array& side, std::span<WireLevel> into) {
    std::size_t count = 0;
    for (auto entry : side) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        std::array<std::string_view, 2> cells{};
        std::size_t n = 0;
        for (auto cell : pair) {
            json::ondemand::value value;
            if (cell.get(value) != simdjson::SUCCESS || n == cells.size()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto text = json::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            cells[n] = *text;
            ++n;
        }
        if (n != cells.size()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = cells[0], .qty = cells[1]};
        ++count;
    }
    return count;
}

std::string_view stream_of(std::string_view channel, std::string_view suffix) noexcept {
    return channel.ends_with(suffix) ? channel.substr(0, channel.size() - suffix.size()) : std::string_view{};
}

std::expected<WireTicker, WireError> read_ticker(json::ondemand::object& row) {
    const auto symbol = json::req_str(row, "symbol");
    const auto mark = json::opt_str(row, "markPrice");
    const auto index = json::opt_str(row, "indexPrice");
    const auto funding = json::opt_str(row, "oneHrFundingRate");
    const auto status = json::opt_str(row, "status");
    if (!symbol.has_value() || !mark.has_value() || !index.has_value() || !funding.has_value() || !status.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireTicker{.symbol = *symbol, .mark = *mark, .index = *index, .funding = *funding, .status = *status};
}

std::expected<WireStat, WireError> read_volume(json::ondemand::object& row) {
    const auto symbol = json::req_str(row, "symbol");
    const auto base = json::opt_str(row, "volume");
    const auto quote = json::opt_str(row, "quoteVolume");
    if (!symbol.has_value() || !base.has_value() || !quote.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireStat{.symbol = *symbol, .first = *base, .second = *quote};
}

std::expected<WireStat, WireError> read_interest(json::ondemand::object& row) {
    const auto symbol = json::req_str(row, "symbol");
    const auto longs = json::opt_str(row, "longOi");
    const auto shorts = json::opt_str(row, "shortOi");
    if (!symbol.has_value() || !longs.has_value() || !shorts.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireStat{.symbol = *symbol, .first = *longs, .second = *shorts};
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return rows_under(body, "symbols", std::span<WireMarket>{storage.markets}, read_market);
}

Channel parse_channel(std::string_view frame) {
    const auto kind = json::guarded([&]() -> std::expected<Channel, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = json::opt_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        if (channel->ends_with("@depth")) {
            return Channel::Depth;
        }
        if (channel->ends_with("@trades")) {
            return Channel::Trades;
        }
        if (!channel->empty()) {
            return Channel::Unknown;
        }
        if (json::has_field(*root, "subscription_outcomes")) {
            return Channel::Subscribed;
        }
        const auto data = json::opt_str(*root, "data");
        return data.has_value() && *data == "pong" ? Channel::Pong : Channel::Unknown;
    });
    return kind.value_or(Channel::Unknown);
}

std::size_t count_refusals(std::string_view frame) {
    const auto refused = json::guarded([&]() -> std::expected<std::size_t, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto outcomes = json::req_array(*root, "subscription_outcomes");
        if (!outcomes.has_value()) {
            return std::unexpected(outcomes.error());
        }
        std::size_t count = 0;
        for (auto entry : *outcomes) {
            json::ondemand::object outcome;
            if (entry.get_object().get(outcome) != simdjson::SUCCESS) {
                continue;
            }
            const auto status = json::opt_str(outcome, "status");
            count += status.has_value() && *status != "registered" ? 1U : 0U;
        }
        return count;
    });
    return refused.value_or(0);
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return json::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = json::req_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        WireBook out{};
        out.symbol = stream_of(*channel, "@depth");
        if (out.symbol.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto bids = json::req_array(*data, "bids");
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto nb = read_side(*bids, storage.bids);
        if (!nb.has_value()) {
            return std::unexpected(nb.error());
        }
        auto asks = json::req_array(*data, "asks");
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto na = read_side(*asks, storage.asks);
        if (!na.has_value()) {
            return std::unexpected(na.error());
        }
        const auto ts = json::opt_u64(*root, "tsMs");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ms = *ts;
        out.bids = std::span<const WireLevel>{storage.bids.data(), *nb};
        out.asks = std::span<const WireLevel>{storage.asks.data(), *na};
        return out;
    });
}

std::expected<WirePrint, WireError> parse_trade(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WirePrint, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = json::req_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto id = json::req_str(*data, "id");
        const auto price = json::req_str(*data, "price");
        const auto qty = json::req_str(*data, "qty");
        const auto side = json::req_str(*data, "side");
        const auto time = json::opt_u64(*data, "time");
        if (!id.has_value() || !price.has_value() || !qty.has_value() || !side.has_value() || !time.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WirePrint out{.symbol = stream_of(*channel, "@trades"),
                      .id = *id,
                      .price = *price,
                      .qty = *qty,
                      .side = *side,
                      .ts_ms = *time};
        if (out.symbol.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body, TickerBuffer& storage) {
    return rows_under(body, "tickers", std::span<WireTicker>{storage.rows}, read_ticker);
}

std::expected<std::span<const WireStat>, WireError> parse_volumes(std::string_view body, StatBuffer& storage) {
    return rows_under(body, "tickers", std::span<WireStat>{storage.rows}, read_volume);
}

std::expected<std::span<const WireStat>, WireError> parse_open_interest(std::string_view body, StatBuffer& storage) {
    return rows_under(body, "ois", std::span<WireStat>{storage.rows}, read_interest);
}

} // namespace raikiri::venues::vest
