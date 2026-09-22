#include <raikiri/venues/backpack/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::backpack {

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
        const auto id = wire::opt_u64(*root, "t");
        const auto engine = wire::opt_u64(*root, "T");
        const auto maker = wire::opt_bool(*root, "m");
        if (!id.has_value() || !engine.has_value() || !maker.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.trade_id = *id;
        out.engine_us = *engine;
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
        const auto rate = wire::opt_str(*root, "f");
        const auto next = wire::opt_u64(*root, "n");
        const auto event = wire::opt_u64(*root, "E");
        if (!index.has_value() || !rate.has_value() || !next.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.index_price = *index;
        out.funding_rate = *rate;
        out.next_funding_ms = *next;
        out.event_us = *event;
        return out;
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                    TickerBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTicker>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_tickers) {
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
            const auto volume = wire::opt_str(obj, "volume");
            const auto quote = wire::opt_str(obj, "quoteVolume");
            if (!volume.has_value() || !quote.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.volume = *volume;
            out.quote_volume = *quote;
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireTicker>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::backpack
