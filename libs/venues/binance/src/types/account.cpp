#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/envelope.hpp>

#include <array>

namespace raikiri::venues::binance {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body,
                                                                      BalanceBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireBalance>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_balances) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            // The asset is read as optional, not required: one nameless row
            // must not cost the account every other balance in the body. The
            // mapper drops it.
            WireBalance out{};
            const std::array<Wanted, 4> wanted{{{"asset", &out.asset},
                                                {"balance", &out.balance},
                                                {"availableBalance", &out.available_balance},
                                                {"crossUnPnl", &out.cross_unpnl}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            const auto update_ms = detail::opt_u64(obj, "updateTime");
            if (!update_ms.has_value()) {
                return std::unexpected(update_ms.error());
            }
            out.update_ms = *update_ms;
            storage.balances[count] = out;
            ++count;
        }
        return std::span<const WireBalance>{storage.balances.data(), count};
    });
}

std::expected<std::span<const WireIncome>, WireError> parse_income(std::string_view body,
                                                                   IncomeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireIncome>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_income_rows) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireIncome out{};
            const std::array<Wanted, 4> wanted{{{"symbol", &out.symbol},
                                                {"incomeType", &out.income_type},
                                                {"income", &out.income},
                                                {"asset", &out.asset}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            out.tran_id = detail::opt_u64(obj, "tranId").value_or(0);
            // A STRING on funding rows ("") and a number on trades, so it is
            // read leniently rather than required.
            out.trade_id = detail::opt_u64(obj, "tradeId").value_or(0);
            const auto time_ms = detail::opt_u64(obj, "time");
            if (!time_ms.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.time_ms = *time_ms;
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireIncome>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::binance
