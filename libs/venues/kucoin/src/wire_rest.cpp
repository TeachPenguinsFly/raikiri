#include "wire_detail.hpp"

namespace raikiri::venues::kucoin {

namespace {

using detail::guarded;
using detail::session;

// A level is [price, size] with both members written as JSON numbers.
std::expected<void, WireError>
read_pair(detail::ondemand::value entry, std::string_view& price, std::string_view& qty) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto raw = detail::raw_of(value);
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            (field == 0 ? price : qty) = *raw;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (const auto ok = read_pair(value, into[count].price, into[count].qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        ++count;
    }
    return count;
}

} // namespace

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::req_str(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
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
            // Optional so a row that names no pair costs that row, not the
            // catalogue; the mapper is what refuses it.
            const auto base_currency = detail::opt_str(obj, "baseCurrency");
            const auto quote_currency = detail::opt_str(obj, "quoteCurrency");
            if (!base_currency.has_value() || !quote_currency.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.base_currency = *base_currency;
            out.quote_currency = *quote_currency;
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

std::expected<WireOrderBook, WireError> parse_level2_snapshot(std::string_view body,
                                                              LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderBook, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireOrderBook, WireError> {
                WireOrderBook out{};
                const auto symbol = detail::req_str(obj, "symbol");
                const auto sequence = detail::opt_i64(obj, "sequence");
                const auto ts = detail::opt_u64(obj, "ts");
                if (!symbol.has_value() || !sequence.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                // Without a sequence the levels cannot be placed in the change
                // chain, and a book that cannot be continued is not a seed.
                if (*sequence <= 0) {
                    return std::unexpected(WireError::MissingField);
                }
                out.symbol = *symbol;
                out.sequence = *sequence;
                out.ts_ns = *ts;
                const auto bids = read_levels(obj, "bids", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(obj, "asks", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                out.bids = {storage.bids.data(), *bids};
                out.asks = {storage.asks.data(), *asks};
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
