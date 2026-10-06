#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bingx {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        const auto read_one = [&](wire::ondemand::object& obj) -> std::expected<void, WireError> {
            WireTrade out{};
            using Text = std::expected<std::string_view, WireError>;
            Text symbol{std::unexpected(WireError::MissingField)};
            Text price{std::unexpected(WireError::MissingField)};
            Text qty{std::unexpected(WireError::MissingField)};
            std::expected<std::uint64_t, WireError> trade_ms{0};
            std::expected<bool, WireError> maker{false};
            const auto walked = wire::for_each_field(obj, [&](std::string_view key, wire::ondemand::value& value) {
                if (key == "s") {
                    symbol = wire::str_of(value);
                } else if (key == "p") {
                    price = wire::str_of(value);
                } else if (key == "q") {
                    qty = wire::str_of(value);
                } else if (key == "T") {
                    trade_ms = wire::u64_of(value);
                } else if (key == "m") {
                    maker = wire::bool_of(value);
                }
            });
            if (!walked.has_value()) {
                return std::unexpected(walked.error());
            }
            for (const auto* text : {&symbol, &price, &qty}) {
                if (!text->has_value() || (*text)->empty()) {
                    return std::unexpected(WireError::MissingField);
                }
            }
            out.symbol = *symbol;
            out.price = *price;
            out.qty = *qty;
            if (!trade_ms.has_value() || !maker.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.trade_ms = *trade_ms;
            out.buyer_is_maker = *maker;
            storage.trades[count] = out;
            ++count;
            return {};
        };

        // swap sends an array and spot one object; type() peeks without consuming
        bool seen = false;
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view key, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (key != "data" || seen) {
                    return {};
                }
                seen = true;
                wire::ondemand::json_type type{};
                if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                if (type != wire::ondemand::json_type::array) {
                    wire::ondemand::object single;
                    if (const auto ec = value.get_object().get(single); ec != simdjson::SUCCESS) {
                        return std::unexpected(wire::map_error(ec));
                    }
                    return read_one(single);
                }
                wire::ondemand::array array;
                if (const auto ec = value.get_array().get(array); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                for (auto entry : array) {
                    if (count == k_max_trades) {
                        return std::unexpected(WireError::TooManyEntries);
                    }
                    wire::ondemand::object obj;
                    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                        return std::unexpected(wire::map_error(ec));
                    }
                    if (const auto one = read_one(obj); !one.has_value()) {
                        return std::unexpected(one.error());
                    }
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (!seen) {
            return std::unexpected(WireError::MissingField);
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
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireMarkPrice out{};
        const auto symbol = wire::req_str(*data, "s");
        const auto mark = wire::req_str(*data, "p");
        if (!symbol.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.mark_price = *mark;
        const auto event = wire::opt_u64(*data, "E");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        out.event_ms = *event;
        return out;
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireTicker out{};
        const auto symbol = wire::req_str(*data, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        // scalars: perp quotes them and spot does not; absent columns read empty
        const auto volume = opt_scalar(*data, "v");
        const auto quote_volume = opt_scalar(*data, "q");
        const auto event = opt_ms_any(*data, "E");
        if (!volume.has_value() || !quote_volume.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.volume = *volume;
        out.quote_volume = *quote_volume;
        out.event_ms = *event;
        return out;
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_spot_tickers(std::string_view body,
                                                                         SpotTickerBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTicker>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTicker out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            const auto volume = opt_scalar(obj, "volume");
            const auto quote_volume = opt_scalar(obj, "quoteVolume");
            const auto close = opt_ms_any(obj, "closeTime");
            if (!volume.has_value() || !quote_volume.has_value() || !close.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.volume = *volume;
            out.quote_volume = *quote_volume;
            out.event_ms = *close;
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireTicker>{storage.rows.data(), count};
    });
}

std::expected<WirePremiumIndex, WireError> parse_premium_index(std::string_view body) {
    return guarded([&]() -> std::expected<WirePremiumIndex, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WirePremiumIndex out{};
        const auto symbol = wire::req_str(*data, "symbol");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        const auto mark = wire::opt_str(*data, "markPrice");
        const auto index = wire::opt_str(*data, "indexPrice");
        const auto rate = wire::opt_str(*data, "lastFundingRate");
        if (!mark.has_value() || !index.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.mark_price = *mark;
        out.index_price = *index;
        out.last_funding_rate = *rate;
        const auto next = wire::opt_u64(*data, "nextFundingTime");
        const auto hours = wire::opt_i64(*data, "fundingIntervalHours");
        if (!next.has_value() || !hours.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_funding_ms = *next;
        out.funding_interval_hours = *hours;
        return out;
    });
}

} // namespace raikiri::venues::bingx
