#include <raikiri/venues/upbit/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::upbit {

namespace {

// a number's token, a string's contents, or empty for null and absent
std::string_view token(wire::ondemand::object& obj, std::string_view key) {
    auto raw = wire::opt_raw(obj, key).value_or(std::string_view{});
    if (raw == "null") {
        return {};
    }
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw = raw.substr(1, raw.size() - 2);
    }
    return raw;
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

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return read_rows(body, storage.rows, [](wire::ondemand::object& obj, WireMarket& row) {
        row.market = wire::opt_str(obj, "market").value_or(std::string_view{});
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body, TickerBuffer& storage) {
    return read_rows(body, storage.rows, [](wire::ondemand::object& obj, WireTicker& row) {
        row.market = wire::opt_str(obj, "market").value_or(std::string_view{});
        row.trade_price = token(obj, "trade_price");
        row.turnover_24h = token(obj, "acc_trade_price_24h");
        row.volume_24h = token(obj, "acc_trade_volume_24h");
        row.ts_ms = wire::opt_u64(obj, "timestamp").value_or(0);
    });
}

std::optional<WireRefusal> parse_refusal(std::string_view body) {
    using Found = std::optional<WireRefusal>;
    const auto found = wire::guarded([&]() -> std::expected<Found, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        wire::ondemand::object error;
        if (wire::find(*root, "error").get_object().get(error) != simdjson::SUCCESS) {
            return Found{};
        }
        return WireRefusal{.name = token(error, "name"), .message = token(error, "message")};
    });
    return found.value_or(Found{});
}

Kind parse_kind(std::string_view frame) {
    const auto kind = wire::guarded([&]() -> std::expected<Kind, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = wire::opt_str(*root, "type").value_or(std::string_view{});
        if (type == "orderbook") {
            return Kind::Orderbook;
        }
        if (type == "trade") {
            return Kind::Trade;
        }
        return wire::has_field(*root, "error") ? Kind::Refusal : Kind::Unknown;
    });
    return kind.value_or(Kind::Unknown);
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireBook out{};
        out.code = wire::opt_str(*root, "code").value_or(std::string_view{});
        out.ts = wire::opt_u64(*root, "timestamp").value_or(0);
        out.snapshot = wire::opt_str(*root, "stream_type").value_or(std::string_view{}) == "SNAPSHOT";
        auto units = wire::req_array(*root, "orderbook_units");
        if (!units.has_value()) {
            return std::unexpected(units.error());
        }
        std::size_t count = 0;
        for (auto entry : *units) {
            if (count == storage.bids.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object unit;
            if (const auto ec = entry.get_object().get(unit); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            storage.asks[count] = {.price = token(unit, "ask_price"), .qty = token(unit, "ask_size")};
            storage.bids[count] = {.price = token(unit, "bid_price"), .qty = token(unit, "bid_size")};
            ++count;
        }
        out.bids = std::span<const WireLevel>{storage.bids.data(), count};
        out.asks = std::span<const WireLevel>{storage.asks.data(), count};
        return out;
    });
}

std::expected<WireTrade, WireError> parse_trade(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WireTrade, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTrade out{};
        out.code = wire::opt_str(*root, "code").value_or(std::string_view{});
        out.price = token(*root, "trade_price");
        out.qty = token(*root, "trade_volume");
        const auto side = wire::opt_str(*root, "ask_bid").value_or(std::string_view{});
        if (side != "ASK" && side != "BID") {
            return std::unexpected(WireError::MissingField);
        }
        out.aggressor_bought = side == "BID";
        out.ts_ms = wire::opt_u64(*root, "trade_timestamp").value_or(0);
        out.sequential_id = wire::opt_u64(*root, "sequential_id").value_or(0);
        out.snapshot = wire::opt_str(*root, "stream_type").value_or(std::string_view{}) == "SNAPSHOT";
        return out;
    });
}

} // namespace raikiri::venues::upbit
