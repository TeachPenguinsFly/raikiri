#include "wire_detail.hpp"

namespace raikiri::venues::bingx {

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // Required, not optional: zero is success, so a body whose envelope
        // cannot be read must never be mistaken for a body that said 0.
        const auto code = detail::req_i64(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return RestEnvelope{.code = *code, .msg = *msg};
    });
}

std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                        ContractBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireContract out{};
            const auto symbol = detail::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            // The precisions ARE the steps (see wire.hpp), so a row missing
            // one fails the load rather than becoming a tick size of 1.
            const auto price_precision = detail::req_i64(obj, "pricePrecision");
            const auto quantity_precision = detail::req_i64(obj, "quantityPrecision");
            const auto status = detail::opt_i64(obj, "status");
            if (!price_precision.has_value() || !quantity_precision.has_value() ||
                !status.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price_precision = *price_precision;
            out.quantity_precision = *quantity_precision;
            out.status = *status;
            for (const auto& [key, field] : {std::pair{"size", &out.size},
                                             std::pair{"tradeMinQuantity", &out.trade_min_quantity},
                                             std::pair{"tradeMinUSDT", &out.trade_min_usdt}}) {
                const auto value = detail::opt_raw(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            storage.contracts[count] = out;
            ++count;
        }
        return std::span<const WireContract>{storage.contracts.data(), count};
    });
}

std::expected<WirePremiumIndex, WireError> parse_premium_index(std::string_view body) {
    return guarded([&]() -> std::expected<WirePremiumIndex, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // Scoped to a symbol, `data` is an object; unscoped it is an array of
        // every contract, ~300 KiB to learn one rate.
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

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto ts = detail::opt_u64(*data, "serverTime");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::bingx
