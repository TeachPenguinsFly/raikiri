#pragma once

#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// the futures account, one settle currency at a time
struct WireFuturesAccount {
    std::string_view currency;
    std::string_view total;
    std::string_view available;
    std::string_view unrealised_pnl;
    std::uint64_t update_s{0};
};

[[nodiscard]] std::expected<WireFuturesAccount, WireError>
parse_futures_account(std::string_view body);

// the private socket needs this user id first in every subscribe payload or streams nothing
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_account_user_id(std::string_view body);

// time is in seconds here; change is signed as the account moved
struct WireAccountBookRow {
    std::string_view contract;
    std::string_view type; // fund | fee | dnw | pnl | ...
    std::string_view change;
    std::string_view text;
    // a string, while time is a number
    std::string_view id;
    // the trade a fee row was charged on; empty on fund rows
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
