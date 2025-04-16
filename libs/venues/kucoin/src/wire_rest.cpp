#include "wire_detail.hpp"

namespace raikiri::venues::kucoin {

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
        const auto code = detail::req_str(*root, "code");
        // `msg` appears only on failures and an error body carries no `data`,
        // so the absence of either proves nothing.
        const auto msg = detail::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        // No KuCoin code is zero, so a code that will not parse cannot decay
        // into one and be mistaken for success.
        const auto value = detail::digits_to_u64(*code);
        if (value == 0) {
            return std::unexpected(WireError::WrongType);
        }
        return RestEnvelope{.code = static_cast<std::int64_t>(value), .msg = *msg};
    });
}

std::expected<WireBullet, WireError> parse_bullet(std::string_view body) {
    return guarded([&]() -> std::expected<WireBullet, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireBullet, WireError> {
                WireBullet out{};
                const auto token = detail::req_str(obj, "token");
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                out.token = *token;
                auto servers = detail::req_array(obj, "instanceServers");
                if (!servers.has_value()) {
                    return std::unexpected(servers.error());
                }
                // Taking the first is the documented client behaviour; an
                // empty list is a refusal, not a connectable default.
                for (auto entry : *servers) {
                    detail::ondemand::object server;
                    if (const auto ec = entry.get_object().get(server); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    const auto endpoint = detail::req_str(server, "endpoint");
                    const auto interval = detail::opt_u64(server, "pingInterval");
                    const auto timeout = detail::opt_u64(server, "pingTimeout");
                    if (!endpoint.has_value() || !interval.has_value() || !timeout.has_value()) {
                        return std::unexpected(WireError::MissingField);
                    }
                    out.endpoint = *endpoint;
                    out.ping_interval_ms = *interval;
                    out.ping_timeout_ms = *timeout;
                    return out;
                }
                return std::unexpected(WireError::MissingField);
            });
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
            if (count == k_max_contracts) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireContract out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto status = detail::req_str(obj, "status");
            const auto type = detail::req_str(obj, "type");
            if (!symbol.has_value() || !status.has_value() || !type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.status = *status;
            out.type = *type;
            // The steps are bare JSON numbers in Java's formatting, so
            // "1.0E-10" and "0.1" both appear; the mapper owns that.
            for (const auto& [key, field] : {std::pair{"tickSize", &out.tick_size},
                                             std::pair{"lotSize", &out.lot_size},
                                             std::pair{"multiplier", &out.multiplier},
                                             std::pair{"indexPriceTickSize", &out.index_tick_size},
                                             std::pair{"maxOrderQty", &out.max_order_qty}}) {
                const auto value = detail::opt_decimal(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto granularity = detail::opt_u64(obj, "fundingRateGranularity");
            if (!granularity.has_value()) {
                return std::unexpected(granularity.error());
            }
            out.funding_granularity_ms = *granularity;
            storage.contracts[count] = out;
            ++count;
        }
        return std::span<const WireContract>{storage.contracts.data(), count};
    });
}

std::expected<WireFundingRate, WireError> parse_funding_current(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                // `symbol` names the funding index, not the contract, so the
                // caller pairs the result with the symbol it asked for.
                const auto rate = detail::req_decimal(obj, "value");
                const auto predicted = detail::opt_decimal(obj, "predictedValue");
                if (!rate.has_value() || !predicted.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.rate = *rate;
                out.predicted_rate = *predicted;
                const auto granularity = detail::opt_u64(obj, "granularity");
                const auto funding = detail::opt_u64(obj, "fundingTime");
                const auto ts = detail::opt_u64(obj, "timePoint");
                if (!granularity.has_value() || !funding.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.granularity_ms = *granularity;
                out.next_funding_ms = *funding;
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // `data` is a bare number on this endpoint, not the object every other
        // read wraps its payload in.
        const auto ts = detail::opt_u64(*root, "data");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::kucoin
