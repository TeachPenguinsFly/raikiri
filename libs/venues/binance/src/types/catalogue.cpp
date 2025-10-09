#include <raikiri/venues/binance/catalogue.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::binance {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

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
            const auto base_asset = detail::opt_str(obj, "baseAsset");
            const auto quote_asset = detail::opt_str(obj, "quoteAsset");
            if (!contract.has_value() || !status.has_value() || !base_asset.has_value() ||
                !quote_asset.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.contract_type = *contract;
            out.status = *status;
            out.base_asset = *base_asset;
            out.quote_asset = *quote_asset;
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

} // namespace raikiri::venues::binance
