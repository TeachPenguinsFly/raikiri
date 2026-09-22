#include <raikiri/venues/xt/account.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::xt {

namespace {

using wire::guarded;
using wire::session;

std::expected<void, WireError> read_futures_balance(wire::ondemand::object& obj,
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

std::expected<void, WireError> read_bill(wire::ondemand::object& obj, WireBill& out) {
    for (const auto& [key, field] : {std::pair{"id", &out.id},
                                     std::pair{"symbol", &out.symbol},
                                     std::pair{"coin", &out.coin},
                                     std::pair{"type", &out.type},
                                     std::pair{"amount", &out.amount},
                                     std::pair{"side", &out.side}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    const auto created = opt_ms(obj, "createdTime");
    if (!created.has_value()) {
        return std::unexpected(created.error());
    }
    out.created_ms = *created;
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

std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                BillBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireBill>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WireBill>(
            *array, storage.bills, [](auto& obj, WireBill& row) { return read_bill(obj, row); });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireBill>{storage.bills.data(), *count};
    });
}

} // namespace raikiri::venues::xt
