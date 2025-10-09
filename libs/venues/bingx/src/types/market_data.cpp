#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bingx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        const auto read_one = [&](detail::ondemand::object& obj) -> std::expected<void, WireError> {
            WireTrade out{};
            const auto symbol = detail::req_str(obj, "s");
            const auto price = detail::req_str(obj, "p");
            const auto qty = detail::req_str(obj, "q");
            if (!symbol.has_value() || !price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.price = *price;
            out.qty = *qty;
            const auto trade_ms = detail::opt_u64(obj, "T");
            const auto maker = detail::opt_bool(obj, "m");
            if (!trade_ms.has_value() || !maker.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.trade_ms = *trade_ms;
            out.buyer_is_maker = *maker;
            storage.trades[count] = out;
            ++count;
            return {};
        };

        // The swap tree sends an array of prints under "data"; the spot tree
        // sends ONE object under the same key. Same envelope, same field
        // names, different arity — so the shape is read off the raw text and
        // then decoded once. Asking for an array and falling back on failure
        // does not work: a refused type conversion leaves the On-Demand
        // document somewhere a rewind cannot recover.
        const auto raw = detail::opt_raw(*root, "data");
        if (!raw.has_value()) {
            return std::unexpected(raw.error());
        }
        const auto text = detail::trim(*raw);
        root->reset();
        if (text.starts_with('[')) {
            auto array = detail::req_array(*root, "data");
            if (!array.has_value()) {
                return std::unexpected(array.error());
            }
            for (auto entry : *array) {
                if (count == k_max_trades) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                detail::ondemand::object obj;
                if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                if (const auto one = read_one(obj); !one.has_value()) {
                    return std::unexpected(one.error());
                }
            }
            return std::span<const WireTrade>{storage.trades.data(), count};
        }
        auto single = detail::req_object(*root, "data");
        if (!single.has_value()) {
            return std::unexpected(single.error());
        }
        if (const auto one = read_one(*single); !one.has_value()) {
            return std::unexpected(one.error());
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
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireMarkPrice out{};
        const auto symbol = detail::req_str(*data, "s");
        const auto mark = detail::req_str(*data, "p");
        if (!symbol.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.mark_price = *mark;
        const auto event = detail::opt_u64(*data, "E");
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
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireTicker out{};
        const auto symbol = detail::req_str(*data, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        // Scalars, not strings: the perpetual tree quotes both numbers and
        // spot sends them bare. Optional on top of that — an absent column
        // reads empty and leaves the rest of the frame usable.
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

std::expected<WirePremiumIndex, WireError> parse_premium_index(std::string_view body) {
    return guarded([&]() -> std::expected<WirePremiumIndex, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WirePremiumIndex out{};
        const auto symbol = detail::req_str(*data, "symbol");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        const auto mark = detail::opt_str(*data, "markPrice");
        const auto index = detail::opt_str(*data, "indexPrice");
        const auto rate = detail::opt_str(*data, "lastFundingRate");
        if (!mark.has_value() || !index.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.mark_price = *mark;
        out.index_price = *index;
        out.last_funding_rate = *rate;
        const auto next = detail::opt_u64(*data, "nextFundingTime");
        const auto hours = detail::opt_i64(*data, "fundingIntervalHours");
        if (!next.has_value() || !hours.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_funding_ms = *next;
        out.funding_interval_hours = *hours;
        return out;
    });
}

} // namespace raikiri::venues::bingx
