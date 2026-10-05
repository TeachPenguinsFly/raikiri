#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePrint>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;
        auto array = wire::req_array(obj, "d");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_prints) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object item;
            if (const auto ec = entry.get_object().get(item); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WirePrint print{};
            std::expected<std::string_view, WireError> price{std::unexpected(WireError::MissingField)};
            std::expected<std::string_view, WireError> qty{std::unexpected(WireError::MissingField)};
            std::expected<std::string_view, WireError> id{std::string_view{}};
            std::expected<std::uint64_t, WireError> trade_ms{0};
            std::expected<bool, WireError> maker{false};
            const auto walked = wire::for_each_field(item, [&](std::string_view key, wire::ondemand::value& value) {
                if (key == "p") {
                    price = wire::str_of(value);
                } else if (key == "q") {
                    qty = wire::str_of(value);
                } else if (key == "t") {
                    id = wire::str_of(value);
                } else if (key == "T") {
                    trade_ms = wire::u64_of(value);
                } else if (key == "m") {
                    maker = wire::bool_of(value);
                }
            });
            if (!walked.has_value()) {
                return std::unexpected(walked.error());
            }
            for (auto* text : {&price, &qty}) {
                if (text->has_value() && (*text)->empty()) {
                    *text = std::unexpected(WireError::MissingField);
                }
            }
            if (!price.has_value()) {
                return std::unexpected(price.error());
            }
            print.price = *price;
            if (!qty.has_value()) {
                return std::unexpected(qty.error());
            }
            print.qty = *qty;
            if (!id.has_value()) {
                return std::unexpected(id.error());
            }
            print.trade_id = *id;
            if (!trade_ms.has_value()) {
                return std::unexpected(trade_ms.error());
            }
            print.trade_ms = *trade_ms;
            if (!maker.has_value()) {
                return std::unexpected(maker.error());
            }
            print.buyer_is_maker = *maker;
            storage.prints[count] = print;
            ++count;
        }
        return std::span<const WirePrint>{storage.prints.data(), count};
    });
}

std::uint64_t newest_trade_ms(std::span<const WirePrint> prints) noexcept {
    std::uint64_t newest = 0;
    for (const auto& print : prints) {
        newest = std::max(newest, print.trade_ms);
    }
    return newest;
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;
        WireTicker out{};
        const auto symbol = wire::req_str(obj, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        obj.reset();
        const auto event_ms = wire::opt_u64(obj, "E");
        if (!event_ms.has_value()) {
            return std::unexpected(event_ms.error());
        }
        out.event_ms = *event_ms;

        obj.reset();
        auto array = wire::req_array(obj, "d");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        // one element per frame; a second would belong to another symbol
        bool filled = false;
        for (auto entry : *array) {
            if (filled) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object item;
            if (const auto ec = entry.get_object().get(item); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            for (const auto& [key, field] : {std::pair{"v", &out.volume}, std::pair{"q", &out.quote_volume}}) {
                item.reset();
                const auto value = wire::opt_str(item, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            item.reset();
            const auto close_ms = wire::opt_u64(item, "C");
            if (!close_ms.has_value()) {
                return std::unexpected(close_ms.error());
            }
            out.close_ms = *close_ms;
            filled = true;
        }
        if (!filled) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WirePremium>, WireError> parse_premium_index(std::string_view body,
                                                                           PremiumBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePremium>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto entry : *root) {
            if (count == k_max_premium_rows) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object item;
            if (const auto ec = entry.get_object().get(item); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WirePremium row{};
            const auto symbol = wire::req_str(item, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            row.symbol = *symbol;
            for (const auto& [key, field] : {std::pair{"markPrice", &row.mark_price},
                                             std::pair{"lastFundingRate", &row.funding_rate},
                                             std::pair{"forecastFundingRate", &row.forecast_rate}}) {
                item.reset();
                const auto value = wire::opt_str(item, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            item.reset();
            const auto next = wire::opt_u64(item, "nextFundingTime");
            if (!next.has_value()) {
                return std::unexpected(next.error());
            }
            row.next_funding_ms = *next;
            item.reset();
            const auto stamped = wire::opt_u64(item, "time");
            if (!stamped.has_value()) {
                return std::unexpected(stamped.error());
            }
            row.event_ms = *stamped;
            storage.rows[count] = row;
            ++count;
        }
        return std::span<const WirePremium>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::weex
