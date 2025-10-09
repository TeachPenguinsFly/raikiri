#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kucoin {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<WireAccountOverview, WireError> parse_account_overview(std::string_view body) {
    return guarded([&]() -> std::expected<WireAccountOverview, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root,
            [](detail::ondemand::object& obj) -> std::expected<WireAccountOverview, WireError> {
                const auto currency = req_decimal(obj, "currency");
                if (!currency.has_value()) {
                    return std::unexpected(currency.error());
                }
                WireAccountOverview out{};
                out.currency = *currency;
                for (const auto& [key, field] :
                     {std::pair{"marginBalance", &out.margin_balance},
                      std::pair{"availableBalance", &out.available_balance},
                      std::pair{"unrealisedPNL", &out.unrealised_pnl}}) {
                    const auto value = opt_decimal(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *field = *value;
                }
                return out;
            });
    });
}

} // namespace raikiri::venues::kucoin
