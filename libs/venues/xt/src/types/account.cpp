#include <raikiri/venues/xt/account.hpp>
#include <raikiri/venues/xt/envelope.hpp>

#include <utility>

namespace raikiri::venues::xt {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<void, WireError> read_futures_balance(detail::ondemand::object& obj,
                                                    WireFuturesBalance& out) {
    for (const auto& [key, field] : {std::pair{"coin", &out.coin},
                                     std::pair{"walletBalance", &out.wallet_balance},
                                     std::pair{"availableBalance", &out.available_balance}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    return {};
}

} // namespace

std::expected<std::span<const WireFuturesBalance>, WireError>
parse_futures_balances(std::string_view body, FuturesBalanceBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFuturesBalance>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        // A coinless row is kept here and dropped by the mapper: this reader
        // reports what XT sent, and the decision that a number with no unit is
        // not a balance belongs where the domain type is built.
        const auto count = read_rows<WireFuturesBalance>(
            *array, storage.balances, [](auto& obj, WireFuturesBalance& row) {
                return read_futures_balance(obj, row);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireFuturesBalance>{storage.balances.data(), *count};
    });
}

} // namespace raikiri::venues::xt
