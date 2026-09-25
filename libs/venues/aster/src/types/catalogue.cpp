#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::aster {

namespace {

using wire::guarded;
using wire::session;

// reads one filter object's field, e.g. PRICE_FILTER -> tickSize
struct FilterWanted {
    std::string_view type;
    std::string_view field;
    std::string_view* into;
};

} // namespace

std::expected<std::span<const WireInstrument>, WireError> parse_exchange_info(std::string_view body,
                                                                              InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto symbols = wire::req_array(*root, "symbols");
        if (!symbols.has_value()) {
            return std::unexpected(symbols.error());
        }

        std::size_t count = 0;
        for (auto entry : *symbols) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireInstrument out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            const auto contract = wire::opt_str(obj, "contractType");
            const auto status = wire::opt_str(obj, "status");
            const auto base_asset = wire::opt_str(obj, "baseAsset");
            const auto quote_asset = wire::opt_str(obj, "quoteAsset");
            if (!contract.has_value() || !status.has_value() || !base_asset.has_value() || !quote_asset.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.contract_type = *contract;
            out.status = *status;
            out.base_asset = *base_asset;
            out.quote_asset = *quote_asset;
            const auto delivery = wire::opt_u64(obj, "deliveryDate");
            if (!delivery.has_value()) {
                return std::unexpected(delivery.error());
            }
            out.delivery_ms = *delivery;

            auto filters = wire::req_array(obj, "filters");
            if (!filters.has_value()) {
                return std::unexpected(filters.error());
            }
            const std::array<FilterWanted, 3> wanted{{
                {"PRICE_FILTER", "tickSize", &out.tick_size},
                {"LOT_SIZE", "stepSize", &out.step_size},
                {"MIN_NOTIONAL", "notional", &out.min_notional},
            }};
            for (auto filter_entry : *filters) {
                wire::ondemand::object filter;
                if (const auto ec = filter_entry.get_object().get(filter); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                const auto type = wire::opt_str(filter, "filterType");
                if (!type.has_value()) {
                    return std::unexpected(type.error());
                }
                for (const auto& want : wanted) {
                    if (*type != want.type) {
                        continue;
                    }
                    const auto value = wire::opt_str(filter, want.field);
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

std::expected<std::span<const WireFundingInfo>, WireError> parse_funding_info(std::string_view body,
                                                                              FundingInfoBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFundingInfo>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_funding_info) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireFundingInfo out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            // a json number; a missing value leaves 0
            out.interval_hours = static_cast<std::uint32_t>(wire::opt_u64(obj, "fundingIntervalHours").value_or(0));
            // absent when unadjusted; lands undef
            out.cap = wire::opt_str(obj, "adjustedFundingRateCap").value_or(std::string_view{});
            out.floor = wire::opt_str(obj, "adjustedFundingRateFloor").value_or(std::string_view{});
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireFundingInfo>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::aster
