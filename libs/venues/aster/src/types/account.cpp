#include <raikiri/venues/aster/account.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <array>
#include <span>

namespace raikiri::venues::aster {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<WireAccount, WireError> parse_account(std::string_view body, BalanceBuffer& storage) {
    return guarded([&]() -> std::expected<WireAccount, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;

        // read before the rows and not through opt_bool, which answers false for an absent field
        WireAccount account{};
        wire::ondemand::value flag;
        const auto found = wire::find(obj, "multiAssetsMargin").get(flag);
        if (found == simdjson::SUCCESS) {
            bool multi = true;
            if (const auto ec = flag.get_bool().get(multi); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            account.multi_assets_margin = multi;
        } else if (found != simdjson::NO_SUCH_FIELD) {
            return std::unexpected(wire::map_error(found));
        }
        // an absent flag keeps the default and does not fail the parse

        auto assets = wire::req_array(obj, "assets");
        if (!assets.has_value()) {
            return std::unexpected(assets.error());
        }
        std::size_t count = 0;
        for (auto entry : *assets) {
            if (count == k_max_balances) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            // optional so one nameless row does not lose the rest; the mapper drops it
            WireAssetBalance out{};
            const std::array<Wanted, 4> wanted{{{"asset", &out.asset},
                                                {"walletBalance", &out.wallet_balance},
                                                {"unrealizedProfit", &out.unrealized_profit},
                                                {"availableBalance", &out.available_balance}}};
            if (const auto ok = read_strings(row, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            const auto update_ms = wire::opt_u64(row, "updateTime");
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

std::expected<std::span<const WireIncome>, WireError> parse_income(std::string_view body, IncomeBuffer& storage) {
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
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireIncome out{};
            const std::array<Wanted, 4> wanted{{{"symbol", &out.symbol},
                                                {"incomeType", &out.income_type},
                                                {"income", &out.income},
                                                {"asset", &out.asset}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            out.tran_id = wire::opt_u64(obj, "tranId").value_or(0);
            // a digit string on commission rows and empty on funding rows, unlike the numeric tranId
            obj.reset();
            out.trade_id = wire::digits_to_u64(wire::opt_str(obj, "tradeId").value_or(std::string_view{}));
            const auto time_ms = wire::opt_u64(obj, "time");
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

} // namespace raikiri::venues::aster
