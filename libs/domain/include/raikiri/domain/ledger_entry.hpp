#pragma once

// funding is booked from this record; a fee is only compared, since the fill already booked it

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <type_traits>

namespace raikiri {

enum class LedgerKind : std::uint8_t {
    Funding = 0, // booked: the venue's record is the only one
    Fee,         // compared: the fill already booked it
};

struct LedgerEntry {
    InstrumentId instrument{};

    // negative paid, positive received, in the settlement currency
    FixedE9 amount{};

    LedgerKind kind{LedgerKind::Funding};
    std::uint8_t pad_[7]{};

    // tells two funding settlements on one instrument apart
    UnixNanos settled_at{};

    // for a fee the venue's trade id; for funding the charge id, often empty
    ExecId reference{};

    UnixNanos ts_init{};
};

static_assert(std::is_trivially_copyable_v<LedgerEntry>);

} // namespace raikiri
