#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

} // namespace

bool is_funding_execution(const WireExecution& wire) noexcept {
    return wire.exec_type == "Funding";
}

std::expected<std::span<const WireLedgerEntry>, WireError>
parse_funding_charges(std::string_view result, LedgerEntryBuffer& storage) {
    return guarded([&] {
        return read_list<WireLedgerEntry>(
            result,
            "list",
            storage.charges,
            k_max_updates,
            [](ondemand::object& obj, WireLedgerEntry& out) -> std::expected<void, WireError> {
                auto id = detail::req_str(obj, "id");
                if (!id.has_value()) {
                    return std::unexpected{id.error()};
                }
                out.id = *id;
                const auto field = [&](std::string_view key) {
                    obj.reset();
                    return detail::opt_str(obj, key).value_or(std::string_view{});
                };
                out.symbol = field("symbol");
                out.type = field("type");
                out.funding = field("funding");
                out.fee = field("fee");
                out.trade_id = field("tradeId");
                out.currency = field("currency");
                out.transaction_ms = detail::digits_to_u64(field("transactionTime"));
                return {};
            });
    });
}

std::expected<std::span<const WireCoinBalance>, WireError>
parse_wallet_balance(std::string_view result, CoinBalanceBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireCoinBalance>, WireError> {
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
        for (auto row : *list) {
            ondemand::object account;
            if (row.get_object().get(account) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            auto coins = detail::req_array(account, "coin");
            if (!coins.has_value()) {
                return std::unexpected{coins.error()};
            }
            for (auto item : *coins) {
                if (count == k_max_coins) {
                    return std::unexpected{WireError::TooManyEntries};
                }
                ondemand::object entry;
                if (item.get_object().get(entry) != simdjson::SUCCESS) {
                    return std::unexpected{WireError::WrongType};
                }
                const auto field = [&](std::string_view key) {
                    entry.reset();
                    return detail::opt_str(entry, key).value_or(std::string_view{});
                };
                WireCoinBalance out{};
                out.coin = field("coin");
                out.wallet_balance = field("walletBalance");
                out.available_to_withdraw = field("availableToWithdraw");
                out.unrealised_pnl = field("unrealisedPnl");
                storage.coins[count++] = out;
            }
            // ONE ACCOUNT: the request names a single accountType and bybit
            // answers with a single row. A second row would be a second wallet
            // reporting the same coin names, and concatenating the two would
            // read as one account holding both.
            break;
        }
        return std::span<const WireCoinBalance>{storage.coins.data(), count};
    });
}

} // namespace raikiri::venues::bybit
