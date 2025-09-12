#pragma once

// The account's own money: what it holds, and what moved through it.
//
// Funding arrives as an EXECUTION row rather than a ledger line on this venue,
// which is why the predicate that recognises one lives here and takes an
// order's type: the row belongs to the order stream, the reading of it belongs
// to the account.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

// One row of the account's transaction log, filtered to settlements.
//
// `id` IS THE IDENTITY. bybit mints a unique one per row, which is what lets a
// charge be booked exactly once — most venues mint nothing for funding, and
// the ledger downstream keys on this.
struct WireLedgerEntry {
    std::string_view id;
    std::string_view symbol;
    std::string_view type;     // SETTLEMENT | TRADE | TRANSFER_IN | ...
    std::string_view funding;  // SIGNED: positive received, negative paid away
    std::string_view fee;      // SIGNED the same way; a rebate stays negative
    std::string_view trade_id; // what a fee row's comparison keys on
    std::string_view currency; // the settlement asset, never assumed
    std::uint64_t transaction_ms{0};
};

struct LedgerEntryBuffer {
    WireLedgerEntry charges[k_max_updates];
};

[[nodiscard]] std::expected<std::span<const WireLedgerEntry>, WireError>
parse_funding_charges(std::string_view result, LedgerEntryBuffer& storage);

// A funding settlement as the private execution topic states it.
//
// SIGNS DIFFER BETWEEN THE TWO SOURCES and normalising is the caller's job:
// here the amount is a FEE (`execFee` negative means the account received),
// while /v5/account/transaction-log states the same charge as `funding`
// positive-for-received. Booking both raw would make the socket and the
// reconciliation disagree about direction on every settlement.
[[nodiscard]] bool is_funding_execution(const WireExecution& wire) noexcept;

// One coin of /v5/account/wallet-balance, from result.list[].coin[]. The
// availability figures bybit deprecated for unified accounts arrive as EMPTY
// STRINGS rather than as absent keys, so an empty view here is the venue
// declining to answer and not a parse that missed the field.
struct WireCoinBalance {
    std::string_view coin;
    std::string_view wallet_balance;
    std::string_view available_to_withdraw;
    std::string_view unrealised_pnl;
};

// A row per coin the account has touched, not per coin bybit lists.
inline constexpr std::size_t k_max_coins = 128;

struct CoinBalanceBuffer {
    std::array<WireCoinBalance, k_max_coins> coins{};
};

[[nodiscard]] std::expected<std::span<const WireCoinBalance>, WireError>
parse_wallet_balance(std::string_view result, CoinBalanceBuffer& storage);

} // namespace raikiri::venues::bybit
