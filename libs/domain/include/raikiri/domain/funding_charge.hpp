#pragma once

// What a venue actually took, or paid, for carrying a perpetual through a
// settlement.
//
// THIS IS NOT A RATE. `FundingUpdate` carries the rate a public stream
// publishes, which is a prediction about a charge; this is the charge, read
// from the venue's own account ledger. Deriving one from the other multiplies
// four independent unknowns — the interval the rate is quoted per, a mark
// price several venues never send, the position size at an instant nobody
// sampled, and whether a settlement occurred at all — and the product lands in
// an append-only ledger where it disagrees with the venue's books forever.
//
// Domain vocabulary rather than exec's, for the reason position_report.hpp
// gives: the adapter that parses it, the channel that carries it and the
// process that books it all need the same words, and transport must not depend
// on the adapter layer.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <type_traits>

namespace raikiri {

struct FundingCharge {
    InstrumentId instrument{};

    // Signed the way the account moved: negative was paid away, positive was
    // received. In the instrument's settlement currency, never converted —
    // an inverse contract settles in base and saying otherwise invents a rate.
    FixedE9 amount{};

    // The venue's settlement stamp. Load-bearing: a charge carries no client
    // order id and most venues mint no id for one, so this is what tells two
    // charges on one instrument apart, and what makes booking it twice
    // impossible.
    UnixNanos settled_at{};

    // The venue's own id for the charge where it mints one — bitmex does, most
    // do not. Empty is normal and is not a defect.
    ExecId reference{};

    UnixNanos ts_init{};
};

static_assert(std::is_trivially_copyable_v<FundingCharge>);

} // namespace raikiri
