#pragma once

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

// one transaction-log entry; every amount is in `currency`, the log's own
struct WireLogEntry {
    std::uint64_t id{0};
    std::string_view type;
    std::string_view instrument;
    std::string_view currency;
    // the session's funding on a perpetual; interest_pl beside it covers only the time since the last trade
    std::string_view total_interest_pl;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_log_entries = 1000;

struct LogBuffer {
    std::array<WireLogEntry, k_max_log_entries> entries{};
};

struct WireLogPage {
    std::span<const WireLogEntry> entries;
    // the next older page; 0 when the venue sent null
    std::uint64_t continuation{0};
};

// private/get_transaction_log: result.logs, newest first
[[nodiscard]] std::expected<WireLogPage, WireError> parse_transaction_log(std::string_view body, LogBuffer& storage);

} // namespace raikiri::venues::deribit
