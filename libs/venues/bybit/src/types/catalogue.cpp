#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit {

namespace {

using wire::guarded;
namespace ondemand = wire::ondemand;

} // namespace

std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view result,
                                                                            InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto& session = wire::session();
        auto root = session.open_object(result);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        auto list = wire::req_array(obj, "list");
        if (!list.has_value()) {
            return std::unexpected{list.error()};
        }

        std::size_t count = 0;
        for (auto item : *list) {
            if (count == k_max_instruments) {
                return std::unexpected{WireError::TooManyEntries};
            }
            ondemand::object entry;
            if (item.get_object().get(entry) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            WireInstrument out{};
            auto symbol = wire::req_str(entry, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected{symbol.error()};
            }
            out.symbol = *symbol;
            const auto field = [&](std::string_view key) {
                entry.reset();
                return wire::opt_str(entry, key).value_or(std::string_view{});
            };
            out.base_coin = field("baseCoin");
            out.quote_coin = field("quoteCoin");
            out.contract_type = field("contractType");
            out.symbol_type = field("symbolType");
            out.status = field("status");
            out.options_type = field("optionsType");
            out.delivery_ms = wire::digits_to_u64(field("deliveryTime"));

            entry.reset();
            if (auto price_filter = wire::req_object(entry, "priceFilter"); price_filter.has_value()) {
                auto filter = *price_filter;
                out.tick_size = wire::opt_str(filter, "tickSize").value_or(std::string_view{});
            }
            entry.reset();
            if (auto lot_filter = wire::req_object(entry, "lotSizeFilter"); lot_filter.has_value()) {
                auto filter = *lot_filter;
                // spot spells these basePrecision and minOrderAmt
                out.qty_step = wire::opt_str(filter, "qtyStep").value_or(std::string_view{});
                if (out.qty_step.empty()) {
                    filter.reset();
                    out.qty_step = wire::opt_str(filter, "basePrecision").value_or(std::string_view{});
                }
                filter.reset();
                out.min_order_qty = wire::opt_str(filter, "minOrderQty").value_or(std::string_view{});
                filter.reset();
                out.min_notional = wire::opt_str(filter, "minNotionalValue").value_or(std::string_view{});
                if (out.min_notional.empty()) {
                    filter.reset();
                    out.min_notional = wire::opt_str(filter, "minOrderAmt").value_or(std::string_view{});
                }
            }
            entry.reset();
            out.funding_interval_min = wire::opt_u64(entry, "fundingInterval").value_or(0);
            out.funding_cap = wire::opt_str(entry, "upperFundingRate").value_or(std::string_view{});
            out.funding_floor = wire::opt_str(entry, "lowerFundingRate").value_or(std::string_view{});
            storage.instruments[count++] = out;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

std::expected<std::string_view, WireError> parse_page_cursor(std::string_view result) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = wire::session().open_object(result);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        const auto cursor = wire::opt_str(obj, "nextPageCursor");
        if (!cursor.has_value()) {
            return std::unexpected{cursor.error()};
        }
        return *cursor;
    });
}

} // namespace raikiri::venues::bybit
