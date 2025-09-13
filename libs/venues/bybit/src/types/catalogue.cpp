#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/envelope.hpp>

namespace raikiri::venues::bybit {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

} // namespace

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view result, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(result);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        auto list = detail::req_array(obj, "list");
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
            auto symbol = detail::req_str(entry, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected{symbol.error()};
            }
            out.symbol = *symbol;
            const auto field = [&](std::string_view key) {
                entry.reset();
                return detail::opt_str(entry, key).value_or(std::string_view{});
            };
            out.base_coin = field("baseCoin");
            out.quote_coin = field("quoteCoin");
            out.contract_type = field("contractType");
            out.status = field("status");

            entry.reset();
            if (auto price_filter = detail::req_object(entry, "priceFilter");
                price_filter.has_value()) {
                auto filter = *price_filter;
                out.tick_size = detail::opt_str(filter, "tickSize").value_or(std::string_view{});
            }
            entry.reset();
            if (auto lot_filter = detail::req_object(entry, "lotSizeFilter");
                lot_filter.has_value()) {
                auto filter = *lot_filter;
                // The spot category spells the same two facts differently:
                // basePrecision for the lot step and minOrderAmt for the
                // minimum notional. Reading only the derivatives names left
                // every spot row without a lot size, and a row with no lot
                // size is dropped — which is why a spot catalogue used to load
                // zero instruments while the caps row claimed the market.
                out.qty_step = detail::opt_str(filter, "qtyStep").value_or(std::string_view{});
                if (out.qty_step.empty()) {
                    filter.reset();
                    out.qty_step =
                        detail::opt_str(filter, "basePrecision").value_or(std::string_view{});
                }
                filter.reset();
                out.min_order_qty =
                    detail::opt_str(filter, "minOrderQty").value_or(std::string_view{});
                filter.reset();
                out.min_notional =
                    detail::opt_str(filter, "minNotionalValue").value_or(std::string_view{});
                if (out.min_notional.empty()) {
                    filter.reset();
                    out.min_notional =
                        detail::opt_str(filter, "minOrderAmt").value_or(std::string_view{});
                }
            }
            storage.instruments[count++] = out;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

} // namespace raikiri::venues::bybit
