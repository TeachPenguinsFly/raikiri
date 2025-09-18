#pragma once

// The identity a funding increment is deduplicated by, in the ledger.
//
// ONE SETTLEMENT REACHES THIS PROCESS TWICE — live off the private socket and
// again from the REST ledger the poller reconciles against — and the venue
// mints a DIFFERENT id on each. Keying on either books the money twice, so the
// key is the settlement itself: the instrument, the second it landed on, and
// the book it was split onto. Seconds rather than nanoseconds because the two
// sources state the stamp at different resolutions, and no instrument settles
// twice in one second.
//
// IT IS BUILT FROM THE PACKED INSTRUMENT ID, NOT THE SYMBOL TEXT. The journal
// field this goes into holds 40 bytes and sbepp's assign_range is an unbounded
// copy behind a debug-only assert, so a 24-character symbol plus a five-digit
// strategy id came to 41 and lost its last byte — which is a strategy id digit.
// Two strategies then shared one key and the ledger's ON CONFLICT DO NOTHING
// silently kept only the first. A packed id is 20 digits at the very most, so
// the longest identity this can produce is 37 bytes and it cannot be truncated.

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::oms {

// Longest possible: 20 digits of packed id, ':', 11 digits of second, ':',
// 5 digits of strategy id.
inline constexpr std::size_t k_max_funding_identity = 38;

// Writes into `into` and returns the view. Empty only when `into` is smaller
// than k_max_funding_identity, which is a programming error rather than a
// runtime state — the caller sizes the buffer from the constant above.
[[nodiscard]] std::string_view funding_identity(InstrumentId instrument,
                                                UnixNanos settled_at,
                                                std::uint16_t strategy_id,
                                                std::span<char> into) noexcept;

} // namespace raikiri::oms
