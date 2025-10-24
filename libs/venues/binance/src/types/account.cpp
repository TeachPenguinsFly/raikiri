#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <array>
#include <span>

namespace raikiri::venues::binance {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<WireAccount, WireError> parse_account(std::string_view body, BalanceBuffer& storage) {
    return guarded([&]() -> std::expected<WireAccount, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;

        // READ BEFORE THE ROWS, and not through opt_bool: that reader answers
        // `false` for a field that is absent, and false is the mode in which
        // the per-asset available figure IS published. A binance that stops
        // sending this — v3 of this path already has — would silently turn
        // one account-wide number into a free balance of every asset.
        WireAccount account{};
        detail::ondemand::value flag;
        const auto found = obj.find_field_unordered("multiAssetsMargin").get(flag);
        if (found == simdjson::SUCCESS) {
            bool multi = true;
            if (const auto ec = flag.get_bool().get(multi); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            account.multi_assets_margin = multi;
        } else if (found != simdjson::NO_SUCH_FIELD) {
            return std::unexpected(detail::map_error(found));
        }
        // An absent flag keeps the default. It is NOT a parse failure: the
        // mode qualifies one of three figures, and refusing the body over it
        // would lose the totals and the pnl beside it as well.

        auto assets = detail::req_array(obj, "assets");
        if (!assets.has_value()) {
            return std::unexpected(assets.error());
        }
        std::size_t count = 0;
        for (auto entry : *assets) {
            if (count == k_max_balances) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            // The asset is read as optional, not required: one nameless row
            // must not cost the account every other balance in the body. The
            // mapper drops it.
            WireAssetBalance out{};
            const std::array<Wanted, 4> wanted{{{"asset", &out.asset},
                                                {"walletBalance", &out.wallet_balance},
                                                {"unrealizedProfit", &out.unrealized_profit},
                                                {"availableBalance", &out.available_balance}}};
            if (const auto ok = read_strings(row, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            const auto update_ms = detail::opt_u64(row, "updateTime");
            if (!update_ms.has_value()) {
                return std::unexpected(update_ms.error());
            }
            out.update_ms = *update_ms;
            storage.balances[count] = out;
            ++count;
        }
        account.assets = std::span<const WireAssetBalance>{storage.balances.data(), count};
        return account;
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
