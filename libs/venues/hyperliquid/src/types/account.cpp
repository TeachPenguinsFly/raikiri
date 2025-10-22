#include <raikiri/venues/hyperliquid/account.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<WireFundingCharge, WireError> read_charge(detail::ondemand::object& obj) {
    WireFundingCharge out{};
    const auto ts = opt_number_ms(obj, "time");
    if (!ts.has_value()) {
        return std::unexpected(ts.error());
    }
    out.time_ms = *ts;
    auto delta = detail::req_object(obj, "delta");
    if (!delta.has_value()) {
        return std::unexpected(delta.error());
    }
    const auto type = detail::req_str(*delta, "type");
    const auto coin = detail::req_str(*delta, "coin");
    const auto usdc = detail::req_str(*delta, "usdc");
    if (!type.has_value() || !coin.has_value() || !usdc.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.type = *type;
    out.coin = *coin;
    out.usdc = *usdc;
    return out;
}

} // namespace

std::expected<std::span<const WireFundingCharge>, WireError>
parse_user_funding(std::string_view body, FundingChargeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFundingCharge>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto row : *root) {
            if (count == k_max_funding_charges) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto charge = read_charge(obj);
            if (!charge.has_value()) {
                return std::unexpected(charge.error());
            }
            storage.charges[count] = *charge;
            ++count;
        }
        return std::span<const WireFundingCharge>{storage.charges.data(), count};
    });
}

} // namespace raikiri::venues::hyperliquid
