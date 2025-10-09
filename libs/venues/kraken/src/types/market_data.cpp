#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kraken {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<WireTrade, WireError> read_trade(detail::ondemand::object& obj) {
    WireTrade out{};
    const auto product = detail::req_str(obj, "product_id");
    const auto uid = detail::opt_str(obj, "uid");
    const auto type = detail::opt_str(obj, "type");
    const auto side = detail::req_str(obj, "side");
    const auto price = req_num(obj, "price");
    const auto qty = req_num(obj, "qty");
    if (!product.has_value() || !uid.has_value() || !type.has_value() || !side.has_value() ||
        !price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.product_id = *product;
    out.uid = *uid;
    out.type = *type;
    out.aggressor_bought = *side == "buy";
    out.price = *price;
    out.qty = *qty;
    const auto time_ms = opt_ms(obj, "time");
    if (!time_ms.has_value()) {
        return std::unexpected(time_ms.error());
    }
    out.time_ms = *time_ms;
    const auto seq = opt_num(obj, "seq");
    if (!seq.has_value()) {
        return std::unexpected(seq.error());
    }
    out.seq = detail::digits_to_u64(*seq);
    return out;
}

} // namespace

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTicker out{};
        const auto product = detail::req_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        out.product_id = *product;
        const auto bid = opt_num(*root, "bid");
        const auto ask = opt_num(*root, "ask");
        const auto bid_size = opt_num(*root, "bid_size");
        const auto ask_size = opt_num(*root, "ask_size");
        if (!bid.has_value() || !ask.has_value() || !bid_size.has_value() ||
            !ask_size.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.bid = *bid;
        out.ask = *ask;
        out.bid_size = *bid_size;
        out.ask_size = *ask_size;
        for (const auto& [key, field] :
             {std::pair{"markPrice", &out.mark_price},
              std::pair{"index", &out.index},
              std::pair{"last", &out.last},
              std::pair{"relative_funding_rate", &out.relative_funding_rate},
              std::pair{"relative_funding_rate_prediction", &out.relative_funding_rate_prediction},
              std::pair{"openInterest", &out.open_interest},
              // Optional like the rest of this list: opt_num answers an empty
              // view for a field the frame omits, so a ticker_lite push still
              // yields its touch instead of failing the whole parse.
              std::pair{"volume", &out.volume},
              std::pair{"volumeQuote", &out.volume_quote}}) {
            const auto value = opt_num(*root, key);
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            *field = *value;
        }
        const auto next_funding = opt_ms(*root, "next_funding_rate_time");
        const auto time_ms = opt_ms(*root, "time");
        const auto suspended = detail::opt_bool(*root, "suspended");
        if (!next_funding.has_value() || !time_ms.has_value() || !suspended.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_funding_rate_time_ms = *next_funding;
        out.time_ms = *time_ms;
        out.suspended = *suspended;
        return out;
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto feed = detail::req_str(*root, "feed");
        if (!feed.has_value()) {
            return std::unexpected(feed.error());
        }
        if (*feed == "trade") {
            const auto one = read_trade(*root);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[0] = *one;
            return std::span<const WireTrade>{storage.trades.data(), 1};
        }
        auto array = detail::req_array(*root, "trades");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto one = read_trade(obj);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[count] = *one;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

} // namespace raikiri::venues::kraken
