#include <array>

#include "wire_detail.hpp"

namespace raikiri::venues::binance {

namespace {

using detail::guarded;
using detail::session;

// Reads one filter object's field, e.g. PRICE_FILTER -> tickSize.
struct FilterWanted {
    std::string_view type;
    std::string_view field;
    std::string_view* into;
};

} // namespace

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return RestError{};
        }
        const auto code = detail::opt_i64(*root, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        if (*code >= 0) {
            return RestError{};
        }
        const auto msg = detail::opt_str(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return RestError{.code = *code, .msg = *msg};
    });
}

std::expected<std::span<const WireInstrument>, WireError>
parse_exchange_info(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto symbols = detail::req_array(*root, "symbols");
        if (!symbols.has_value()) {
            return std::unexpected(symbols.error());
        }

        std::size_t count = 0;
        for (auto entry : *symbols) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireInstrument out{};
            const auto symbol = detail::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            const auto contract = detail::opt_str(obj, "contractType");
            const auto status = detail::opt_str(obj, "status");
            if (!contract.has_value() || !status.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.contract_type = *contract;
            out.status = *status;
            const auto delivery = detail::opt_u64(obj, "deliveryDate");
            if (!delivery.has_value()) {
                return std::unexpected(delivery.error());
            }
            out.delivery_ms = *delivery;

            auto filters = detail::req_array(obj, "filters");
            if (!filters.has_value()) {
                return std::unexpected(filters.error());
            }
            const std::array<FilterWanted, 3> wanted{{
                {"PRICE_FILTER", "tickSize", &out.tick_size},
                {"LOT_SIZE", "stepSize", &out.step_size},
                {"MIN_NOTIONAL", "notional", &out.min_notional},
            }};
            for (auto filter_entry : *filters) {
                detail::ondemand::object filter;
                if (const auto ec = filter_entry.get_object().get(filter);
                    ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                const auto type = detail::opt_str(filter, "filterType");
                if (!type.has_value()) {
                    return std::unexpected(type.error());
                }
                for (const auto& want : wanted) {
                    if (*type != want.type) {
                        continue;
                    }
                    const auto value = detail::opt_str(filter, want.field);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *want.into = *value;
                }
            }
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

std::expected<WirePremiumIndex, WireError> parse_premium_index(std::string_view body) {
    return guarded([&]() -> std::expected<WirePremiumIndex, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WirePremiumIndex out{};
        const auto symbol = detail::req_str(root.value(), "symbol");
        const auto mark = detail::req_str(*root, "markPrice");
        if (!symbol.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.mark_price = *mark;
        const auto index = detail::opt_str(*root, "indexPrice");
        const auto rate = detail::opt_str(*root, "lastFundingRate");
        if (!index.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.index_price = *index;
        out.last_funding_rate = *rate;
        const auto next = detail::opt_u64(*root, "nextFundingTime");
        const auto time = detail::opt_u64(*root, "time");
        if (!next.has_value() || !time.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_funding_ms = *next;
        out.time_ms = *time;
        return out;
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto time = detail::opt_u64(*root, "serverTime");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        if (*time == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *time;
    });
}

} // namespace raikiri::venues::binance
