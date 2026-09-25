#include <raikiri/venues/binance/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::binance {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<WirePrint, WireError> parse_trade(std::string_view data) {
    return guarded([&]() -> std::expected<WirePrint, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WirePrint out{};
        const auto symbol = wire::req_str(*root, "s");
        const auto price = wire::req_str(*root, "p");
        const auto qty = wire::req_str(*root, "q");
        if (!symbol.has_value() || !price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.price = *price;
        out.qty = *qty;
        // a on the aggregate stream, t on the per-trade one; opt_u64 gives 0 for the absent one
        const auto aggregate_id = wire::opt_u64(*root, "a");
        const auto single_id = wire::opt_u64(*root, "t");
        const auto trade_ms = wire::opt_u64(*root, "T");
        const auto maker = wire::opt_bool(*root, "m");
        if (!aggregate_id.has_value() || !single_id.has_value() || !trade_ms.has_value() || !maker.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.trade_id = *aggregate_id != 0 ? *aggregate_id : *single_id;
        out.trade_ms = *trade_ms;
        out.buyer_is_maker = *maker;
        return out;
    });
}

std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view data) {
    return guarded([&]() -> std::expected<WireMarkPrice, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireMarkPrice out{};
        const auto symbol = wire::req_str(*root, "s");
        const auto mark = wire::req_str(*root, "p");
        if (!symbol.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.mark_price = *mark;
        const auto index = wire::opt_str(*root, "i");
        const auto rate = wire::opt_str(*root, "r");
        if (!index.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.index_price = *index;
        out.funding_rate = *rate;
        const auto next = wire::opt_u64(*root, "T");
        const auto event = wire::opt_u64(*root, "E");
        if (!next.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_funding_ms = *next;
        out.event_ms = *event;
        return out;
    });
}

std::expected<std::span<const WireMarkPrice>, WireError> parse_premium_index(std::string_view body,
                                                                             PremiumBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireMarkPrice>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_premium_rows) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireMarkPrice out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.mark_price = wire::opt_str(obj, "markPrice").value_or(std::string_view{});
            out.index_price = wire::opt_str(obj, "indexPrice").value_or(std::string_view{});
            out.funding_rate = wire::opt_str(obj, "lastFundingRate").value_or(std::string_view{});
            out.next_funding_ms = wire::opt_u64(obj, "nextFundingTime").value_or(0);
            out.event_ms = wire::opt_u64(obj, "time").value_or(0);
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireMarkPrice>{storage.rows.data(), count};
    });
}

std::expected<std::span<const WireTicker24h>, WireError> parse_ticker_24h(std::string_view body,
                                                                          Ticker24hBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTicker24h>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_ticker_rows) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTicker24h out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.volume = wire::opt_str(obj, "volume").value_or(std::string_view{});
            out.quote_volume = wire::opt_str(obj, "quoteVolume").value_or(std::string_view{});
            out.close_ms = wire::opt_u64(obj, "closeTime").value_or(0);
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireTicker24h>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::binance
