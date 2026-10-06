#include <raikiri/venues/cryptocom/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include "fields.hpp"

namespace raikiri::venues::cryptocom {

namespace {

template <typename Out, typename Fn> std::expected<Out, WireError> first_row(std::string_view data, Fn&& read) {
    auto array = wire::session().open_array(data);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        Out out{};
        if (const auto done = read(row, out); !done.has_value()) {
            return std::unexpected(done.error());
        }
        return out;
    }
    return std::unexpected(WireError::MissingField);
}

} // namespace

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view data, TradeBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto array = wire::session().open_array(data);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTrade out{};
            const auto read = each_field(row, [&](std::string_view key, ondemand::value& value) -> FieldResult {
                if (key == "d") {
                    return text_into(value, out.trade_id);
                }
                if (key == "t") {
                    return u64_into(value, out.trade_ms);
                }
                if (key == "p") {
                    return text_into(value, out.price);
                }
                if (key == "q") {
                    return text_into(value, out.qty);
                }
                if (key == "s") {
                    return text_into(value, out.side);
                }
                return {};
            });
            if (!read.has_value()) {
                return std::unexpected(read.error());
            }
            if (out.price.empty() || out.qty.empty() || out.side.empty()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.rows.data(), count};
    });
}

namespace {

FieldResult read_ticker(ondemand::object& row, WireTicker& out) {
    return each_field(row, [&](std::string_view key, ondemand::value& value) -> FieldResult {
        if (key == "i") {
            return text_into(value, out.instrument);
        }
        if (key == "v") {
            return text_into(value, out.volume);
        }
        if (key == "vv") {
            return text_into(value, out.quote_volume);
        }
        if (key == "oi") {
            return text_into(value, out.open_interest);
        }
        if (key == "t") {
            return u64_into(value, out.ts_ms);
        }
        return {};
    });
}

} // namespace

std::expected<WireTicker, WireError> parse_ticker(std::string_view data) {
    return wire::guarded([&]() -> std::expected<WireTicker, WireError> {
        return first_row<WireTicker>(data,
                                     [](ondemand::object& row, WireTicker& out) { return read_ticker(row, out); });
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body, TickerBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireTicker>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        auto array = wire::req_array(*result, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTicker out{};
            if (const auto done = read_ticker(row, out); !done.has_value()) {
                return std::unexpected(done.error());
            }
            if (out.instrument.empty()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.rows[count++] = out;
        }
        return std::span<const WireTicker>{storage.rows.data(), count};
    });
}

std::expected<WireValue, WireError> parse_value(std::string_view data) {
    return wire::guarded([&]() -> std::expected<WireValue, WireError> {
        auto out = first_row<WireValue>(data, [](ondemand::object& row, WireValue& value_out) {
            return each_field(row, [&](std::string_view key, ondemand::value& value) -> FieldResult {
                if (key == "v") {
                    return text_into(value, value_out.value);
                }
                if (key == "t") {
                    return u64_into(value, value_out.ts_ms);
                }
                return {};
            });
        });
        if (out.has_value() && out->value.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

} // namespace raikiri::venues::cryptocom
