#pragma once

// One line of the venue's own account ledger: money it took or paid that is not
// the trade itself.
//
// THE TWO KINDS ARE NOT USED THE SAME WAY, and getting that backwards costs
// real money in an append-only ledger:
//
//   Funding — the venue's record is the ONLY record. Nothing else in this
//     process knows what was settled, so it is BOOKED. Deriving it instead from
//     the public rate multiplies four unknowns: the interval the rate is quoted
//     per, a mark price several venues never send, the position size at an
//     instant nobody sampled, and whether a settlement occurred at all.
//
//   Fee — already booked from the fill that incurred it, where the venue states
//     it on the execution itself. This copy exists to be COMPARED against that,
//     and booking it would charge every commission twice.
//
// Domain vocabulary rather than exec's, for the reason position_report.hpp
// gives: the adapter that parses it, the channel that carries it and the
// process that books it all need the same words.

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

    // Signed the way the account moved: negative was paid away, positive was
    // received. In the instrument's settlement currency, never converted — an
    // inverse contract settles in base and saying otherwise invents a rate.
    FixedE9 amount{};

    LedgerKind kind{LedgerKind::Funding};
    std::uint8_t pad_[7]{};

    // The venue's settlement or transaction stamp. Load-bearing for funding:
    // a charge carries no client order id and most venues mint no id for one,
    // so this is what tells two settlements on one instrument apart.
    UnixNanos settled_at{};

    // For a FEE, the venue's id for the TRADE that incurred it — which is what
    // the comparison keys on, because our own booked fee is keyed by the same
    // execution. For funding it is the venue's id for the charge where it mints
    // one, and empty is normal.
    ExecId reference{};

    UnixNanos ts_init{};
};

static_assert(std::is_trivially_copyable_v<LedgerEntry>);

} // namespace raikiri
