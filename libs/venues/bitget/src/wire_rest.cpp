#include "wire_detail.hpp"

namespace raikiri::venues::bitget {

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
            if (count == k_max_contracts) {
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
            // Optional here so the mapper is what refuses a row.
            for (const auto& [key, field] : {std::pair{"baseCoin", &out.base_coin},
                                             std::pair{"quoteCoin", &out.quote_coin},
                                             std::pair{"symbolType", &out.symbol_type},
                                             std::pair{"symbolStatus", &out.symbol_status},
                                             std::pair{"pricePlace", &out.price_place},
                                             std::pair{"priceEndStep", &out.price_end_step},
                                             std::pair{"sizeMultiplier", &out.size_multiplier},
                                             std::pair{"minTradeNum", &out.min_trade_num},
                                             std::pair{"minTradeUSDT", &out.min_trade_usdt},
                                             std::pair{"fundInterval", &out.fund_interval}}) {
                const auto value = detail::opt_str(obj, key);
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
        const auto ts = detail::opt_ms(*data, "serverTime");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

std::expected<WireFundingRate, WireError> parse_current_fund_rate(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                const auto symbol = detail::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                for (const auto& [key, field] :
                     {std::pair{"fundingRate", &out.funding_rate},
                      std::pair{"fundingRateInterval", &out.interval_hours},
                      std::pair{"minFundingRate", &out.min_rate},
                      std::pair{"maxFundingRate", &out.max_rate}}) {
                    const auto value = detail::opt_str(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *field = *value;
                }
                const auto next = detail::opt_ms(obj, "nextUpdate");
                if (!next.has_value()) {
                    return std::unexpected(next.error());
                }
                out.next_update_ms = *next;
                return out;
            });
    });
}

std::expected<WireBooks, WireError> parse_rest_orderbook(std::string_view body,
                                                         LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBooks, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBooks out{};
        out.snapshot = true;
        if (const auto ok = detail::read_book_body(*data, storage, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

} // namespace raikiri::venues::bitget
