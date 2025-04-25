#pragma once

// Plumbing shared by the htx wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM. What differs is
// below — HTX puts prices, sizes and most timestamps in JSON numbers, so the
// raw-token reader is the workhorse here rather than okx's string readers.
//
// simdjson stays confined to this directory; wire.hpp hands out only
// string_view and span.

#include <raikiri/venues/htx/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::htx::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and needs that many readable
// bytes past the document, which an inflated frame does not have. The frame is
// copied once into a thread-lifetime padded buffer and every view simdjson
// produces is translated back to the caller's (rebase) — nothing handed out
// points into scratch the next frame overwrites.
class Session {
public:
    [[nodiscard]] std::expected<ondemand::object, WireError> open_object(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

private:
    simdjson::padded_string scratch_;
    ondemand::parser parser_;
    ondemand::document doc_;
    const char* origin_{nullptr};
    std::size_t origin_size_{0};
};

// One session per thread: rebuilding the structural index allocation per
// frame is the largest avoidable cost simdjson has.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

// The raw JSON text of a field, whatever its type, quotes included. Every
// price and size goes through this: HTX writes them as numbers, and
// "1.00000000E-10" is a spelling only the token preserves.
[[nodiscard]] std::expected<std::string_view, WireError> opt_raw(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> raw_of(ondemand::value value);

// A raw token minus its quotes, if it had any: one reader for `err-code`
// whether HTX wrote "bad-request" or 2011.
[[nodiscard]] std::string_view unquote(std::string_view raw) noexcept;

// Epoch milliseconds as a JSON number; an absent field yields 0, which the
// mapping layer reads as "the venue gave us no time".
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj,
                                                              std::string_view key);

[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);

// Epoch milliseconds as a digit STRING — `funding_time`, `settlement_time` and
// `settlement_date` all take that form. Never parse_decimal_e9: it scales by
// 1e9 and an epoch-ms value overflows int64 on the way.
[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms_str(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);

// "market.BTC-USDT.bbo" and "public.BTC-USDT.funding_rate" both -> "BTC-USDT".
// A contract code carries a hyphen and never a dot, so it is what sits between
// the first and second separator whatever follows ("trade.detail" has a dot of
// its own). Not noexcept only because substr is not; callers are in guarded().
[[nodiscard]] std::string_view code_of(std::string_view topic);

// simdjson's On-Demand API throws through its implicit conversions. This file
// never uses those, but "never" is a claim about code other people will edit,
// and an exception escaping into a gateway holding live orders is expensive.
template <typename Fn> [[nodiscard]] auto guarded(Fn&& fn) -> decltype(fn()) {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return std::unexpected(WireError::NotJson);
    }
}

} // namespace raikiri::venues::htx::detail
