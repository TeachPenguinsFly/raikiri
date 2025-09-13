#pragma once

// The account's own book: what it holds, and every row that moved it.

#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// The futures account, one settle currency at a time. Gate answers this from
// the same endpoint the private socket's user id comes from — the numbers were
// already arriving and being thrown away.
struct WireFuturesAccount {
    std::string_view currency;
    std::string_view total;
    std::string_view available;
    std::string_view unrealised_pnl;
    std::uint64_t update_s{0};
};

[[nodiscard]] std::expected<WireFuturesAccount, WireError>
parse_futures_account(std::string_view body);

// `user` from GET /futures/{settle}/accounts, rendered as digits. The futures
// private socket wants it as the first element of every subscribe payload and
// answers a payload without it with an empty stream rather than an error.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_account_user_id(std::string_view body);

// One row of the futures account book, narrowed to settlements.
//
// TIME IS IN SECONDS here, unlike the millisecond stamps gate's order
// endpoints use. `change` is signed the way the account moved, which is
// already the domain's convention.
struct WireAccountBookRow {
    std::string_view contract;
    std::string_view type; // fund | fee | dnw | pnl | ...
    std::string_view change;
    std::string_view text;
    // A STRING, not a number — gate quotes it as "991" while `time` on the
    // same row is a bare number. Verified against a live settlement row.
    std::string_view id;
    // The trade a `fee` row was charged on; empty on `fund` rows, which have
    // no trade. A STRING here too, like `id`.
    std::string_view trade_id;
    std::uint64_t time_s{0};
};

inline constexpr std::size_t k_max_book_rows = 256;

struct AccountBookBuffer {
    std::array<WireAccountBookRow, k_max_book_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireAccountBookRow>, WireError>
parse_account_book(std::string_view body, AccountBookBuffer& storage);

} // namespace raikiri::venues::gate
