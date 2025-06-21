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

std::expected<WireDepthSnapshot, WireError> parse_depth_snapshot(std::string_view body,
                                                                 LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepthSnapshot, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireDepthSnapshot out{};
        const auto last = detail::opt_u64(*root, "lastUpdateId");
        const auto transaction = detail::opt_u64(*root, "T");
        const auto event = detail::opt_u64(*root, "E");
        if (!last.has_value() || !transaction.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (*last == 0) {
            return std::unexpected(WireError::MissingField); // a seed with no id seeds nothing
        }
        out.last_update_id = *last;
        out.transaction_ms = *transaction;
        out.event_ms = *event;

        const auto bids = detail::read_levels(*root, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = detail::read_levels(*root, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

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
