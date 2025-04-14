#include "wire_detail.hpp"

namespace raikiri::venues::xt {

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
        const auto return_code = detail::req_i64(*root, "returnCode");
        const auto msg = detail::opt_str(*root, "msgInfo");
        if (!return_code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        RestEnvelope out{.return_code = *return_code, .code = {}, .msg = *msg};
        // `error` is null on success and an object on failure; its `code` is
        // the only machine-readable identity a failure has, and it is a string
        // ("invalid_symbol", "400"), so classification works on text.
        if (auto error = detail::req_object(*root, "error"); error.has_value()) {
            const auto code = detail::opt_str(*error, "code");
            const auto detail_msg = detail::opt_str(*error, "msg");
            if (!code.has_value() || !detail_msg.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.code = *code;
            if (!detail_msg->empty()) {
                out.msg = *detail_msg;
            }
        }
        return out;
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = detail::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        WireFundingRate out{};
        const auto symbol = detail::req_str(*result, "symbol");
        const auto rate = detail::opt_raw(*result, "fundingRate");
        if (!symbol.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.symbol = *symbol;
        out.rate_token = *rate;
        const auto next = detail::opt_ms(*result, "nextCollectionTime");
        const auto interval = detail::req_i64(*result, "collectionInternal");
        if (!next.has_value() || !interval.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_collection_ms = *next;
        out.interval_h = *interval;
        return out;
    });
}

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "result");
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
            WireInstrument out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto product_type = detail::req_str(obj, "productType");
            if (!symbol.has_value() || !product_type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.product_type = *product_type;
            for (const auto& [key, field] : {std::pair{"minStepPrice", &out.tick_size},
                                             std::pair{"contractSize", &out.contract_size},
                                             std::pair{"minNotional", &out.min_notional}}) {
                const auto value = detail::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            // Required, not optional: these are what the mapper gates on, and
            // a missing switch defaulting to false would drop the whole
            // catalogue silently instead of reporting a schema change.
            const auto state = detail::req_i64(obj, "state");
            const auto precision = detail::req_i64(obj, "quantityPrecision");
            const auto trade_switch = detail::req_bool(obj, "tradeSwitch");
            const auto open_api = detail::req_bool(obj, "isOpenApi");
            if (!state.has_value() || !precision.has_value() || !trade_switch.has_value() ||
                !open_api.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.state = *state;
            out.qty_precision = *precision;
            out.trade_switch = *trade_switch;
            out.open_api = *open_api;
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
        const auto ts = detail::opt_ms(*root, "result");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::xt
