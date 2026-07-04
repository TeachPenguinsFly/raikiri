#include <raikiri/venues/pacifica/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::pacifica {

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

std::expected<WireMarket, WireError> read_market(json::ondemand::object& row) {
    WireMarket out{};
    const auto symbol = json::req_str(row, "symbol");
    const auto type = json::req_str(row, "instrument_type");
    const auto tick = json::req_str(row, "tick_size");
    const auto lot = json::req_str(row, "lot_size");
    const auto min = json::opt_str(row, "min_order_size");
    if (!symbol.has_value() || !type.has_value() || !tick.has_value() || !lot.has_value() ||
        !min.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = *symbol;
    out.type = *type;
    out.tick = *tick;
    out.lot = *lot;
    out.min_notional = *min;
    json::ondemand::array modes;
    if (row.find_field_unordered("execution_modes").get_array().get(modes) == simdjson::SUCCESS) {
        for (auto entry : modes) {
            json::ondemand::value value;
            if (entry.get(value) != simdjson::SUCCESS) {
                continue;
            }
            if (const auto mode = json::str_of(value); mode.has_value() && *mode == "rfq") {
                out.rfq = true;
            }
        }
    }
    return out;
}

std::expected<std::size_t, WireError> read_side(json::ondemand::array& side,
                                                std::span<WireLevel> into) {
    std::size_t count = 0;
    for (auto entry : side) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::object level;
        if (const auto ec = entry.get_object().get(level); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        const auto price = json::req_str(level, "p");
        const auto qty = json::req_str(level, "a");
        if (!price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = *price, .qty = *qty};
        ++count;
    }
    return count;
}

std::expected<std::uint64_t, WireError> req_id(json::ondemand::object& obj, std::string_view key) {
    const auto value = json::opt_u64(obj, key);
    if (!value.has_value()) {
        return std::unexpected(value.error());
    }
    if (*value == 0) {
        return std::unexpected(WireError::MissingField);
    }
    return *value;
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                    MarketBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(*rows, std::span<WireMarket>{storage.markets}, read_market);
    });
}

Channel parse_channel(std::string_view frame) {
    const auto kind = json::guarded([&]() -> std::expected<Channel, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = json::opt_str(*root, "channel");
        if (!channel.has_value() || channel->empty()) {
            return json::has_field(*root, "error") ? Channel::Error : Channel::Unknown;
        }
        if (*channel == "book") {
            return Channel::Book;
        }
        if (*channel == "bbo") {
            return Channel::Bbo;
        }
        if (*channel == "trades") {
            return Channel::Trades;
        }
        if (*channel == "prices") {
            return Channel::Prices;
        }
        if (*channel == "pong") {
            return Channel::Pong;
        }
        if (*channel == "subscribe") {
            return Channel::Subscribed;
        }
        return Channel::Unknown;
    });
    return kind.value_or(Channel::Unknown);
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return json::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBook out{};
        const auto symbol = json::req_str(*data, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        auto sides = json::req_array(*data, "l");
        if (!sides.has_value()) {
            return std::unexpected(sides.error());
        }
        std::size_t index = 0;
        for (auto entry : *sides) {
            if (index == 2) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::array side;
            if (const auto ec = entry.get_array().get(side); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            auto& into = index == 0 ? storage.bids : storage.asks;
            const auto count = read_side(side, into);
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            (index == 0 ? out.bids : out.asks) = std::span<const WireLevel>{into.data(), *count};
            ++index;
        }
        if (index != 2) {
            return std::unexpected(WireError::ShortArray);
        }
        const auto li = req_id(*data, "li");
        if (!li.has_value()) {
            return std::unexpected(li.error());
        }
        out.li = *li;
        const auto ts = json::opt_u64(*data, "t");
        out.ts_ms = ts.value_or(0);
        return out;
    });
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = json::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto symbol = json::req_str(*data, "s");
        const auto li = req_id(*data, "li");
        const auto ts = json::opt_u64(*data, "t");
        const auto bid = json::req_str(*data, "b");
        const auto bid_qty = json::req_str(*data, "B");
        const auto ask = json::req_str(*data, "a");
        const auto ask_qty = json::req_str(*data, "A");
        if (!symbol.has_value() || !li.has_value() || !bid.has_value() || !bid_qty.has_value() ||
            !ask.has_value() || !ask_qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireBbo{.symbol = *symbol,
                       .li = *li,
                       .ts_ms = ts.value_or(0),
                       .bid = *bid,
                       .bid_qty = *bid_qty,
                       .ask = *ask,
                       .ask_qty = *ask_qty};
    });
}

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                  PrintBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WirePrint>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(
            *rows,
            std::span<WirePrint>{storage.prints},
            [](json::ondemand::object& row) -> std::expected<WirePrint, WireError> {
                const auto history = req_id(row, "h");
                const auto symbol = json::req_str(row, "s");
                const auto qty = json::req_str(row, "a");
                const auto price = json::req_str(row, "p");
                const auto direction = json::req_str(row, "d");
                const auto ts = json::opt_u64(row, "t");
                if (!history.has_value() || !symbol.has_value() || !qty.has_value() ||
                    !price.has_value() || !direction.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                return WirePrint{.symbol = *symbol,
                                 .history_id = *history,
                                 .ts_ms = ts.value_or(0),
                                 .price = *price,
                                 .qty = *qty,
                                 .direction = *direction};
            });
    });
}

std::expected<std::span<const WirePrice>, WireError> parse_prices(std::string_view frame,
                                                                  PriceBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WirePrice>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_rows(
            *rows,
            std::span<WirePrice>{storage.rows},
            [](json::ondemand::object& row) -> std::expected<WirePrice, WireError> {
                const auto symbol = json::req_str(row, "symbol");
                const auto funding = json::opt_str(row, "funding");
                const auto next = json::opt_str(row, "next_funding");
                const auto mark = json::opt_str(row, "mark");
                const auto volume = json::opt_str(row, "volume_24h");
                const auto ts = json::opt_u64(row, "timestamp");
                if (!symbol.has_value() || !funding.has_value() || !next.has_value() ||
                    !mark.has_value() || !volume.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                return WirePrice{.symbol = *symbol,
                                 .funding = *funding,
                                 .next_funding = *next,
                                 .mark = *mark,
                                 .volume_24h = *volume,
                                 .ts_ms = ts.value_or(0)};
            });
    });
}

} // namespace raikiri::venues::pacifica
