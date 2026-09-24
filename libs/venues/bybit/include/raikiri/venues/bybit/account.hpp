#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

// transaction log row; bybit mints a unique id per row and the ledger keys on it
struct WireLedgerEntry {
    std::string_view id;
    std::string_view symbol;
    std::string_view type;     // SETTLEMENT | TRADE | TRANSFER_IN | ...
    std::string_view funding; // signed: positive received, negative paid
    std::string_view fee; // signed the same way; a rebate stays negative
    std::string_view trade_id; // what a fee row's comparison keys on
    std::string_view currency; // the settlement asset, never assumed
    std::uint64_t transaction_ms{0};
};

struct LedgerEntryBuffer {
    WireLedgerEntry charges[k_max_updates];
};

[[nodiscard]] std::expected<std::span<const WireLedgerEntry>, WireError>
parse_funding_charges(std::string_view result, LedgerEntryBuffer& storage);

// execFee is negative when received, transaction-log funding positive; the caller normalises
[[nodiscard]] bool is_funding_execution(const WireExecution& wire) noexcept;

// deprecated availability fields arrive as empty strings on unified accounts
struct WireCoinBalance {
    std::string_view coin;
    std::string_view wallet_balance;
    std::string_view available_to_withdraw;
    std::string_view unrealised_pnl;
};

// a row per coin the account has touched
inline constexpr std::size_t k_max_coins = 128;

struct CoinBalanceBuffer {
    std::array<WireCoinBalance, k_max_coins> coins{};
};

[[nodiscard]] std::expected<std::span<const WireCoinBalance>, WireError>
parse_wallet_balance(std::string_view result, CoinBalanceBuffer& storage);

} // namespace raikiri::venues::bybit
