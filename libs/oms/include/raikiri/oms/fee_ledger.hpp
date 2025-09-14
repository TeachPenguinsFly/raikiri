#pragma once

// What this process booked as a fee, kept until the venue quotes it back.
//
// A fee is the one figure both sides state independently: we derive it from
// the fill, the venue states it on its own ledger, and the two agreeing is the
// only evidence that the fill was read correctly at all. Three answers matter
// and they are not the same answer:
//
//   Agreed     — the two figures match to the last unit
//   Disagreed  — both exist and differ. A break, and money
//   Unmatched  — the venue charged against a fill this process never booked,
//                which is either a fill we missed or a venue quoting a
//                different id here than on the execution
//
// The verdict is returned rather than logged, because a thing that decides is
// testable and a thing that prints is not.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/ledger_entry.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace raikiri::oms {

enum class FeeVerdict : std::uint8_t { Agreed, Disagreed, Unmatched };

struct FeeCheck {
    FeeVerdict verdict{FeeVerdict::Unmatched};
    // What this process booked. UNDEF when the verdict is Unmatched — not
    // zero, which would read as a fee we stated and the venue disputed.
    FixedE9 booked{};
};

class FeeLedger {
public:
    // Keyed on the venue's own id for the execution, because that is what its
    // ledger quotes back when it states the same fee.
    void book(std::string_view reference, FixedE9 amount, UnixNanos at);

    [[nodiscard]] FeeCheck reconcile(const LedgerEntry& charge) const;

    // Dropped once older than the window a venue's ledger still reaches back
    // over: past that the venue will never quote the fee again, so keeping the
    // memo only grows the map.
    void prune(UnixNanos now, std::uint64_t max_age_ns);

    [[nodiscard]] std::size_t size() const noexcept { return booked_.size(); }

private:
    struct Memo {
        FixedE9 amount{};
        UnixNanos booked_at{};
    };

    std::unordered_map<std::string, Memo> booked_;
};

} // namespace raikiri::oms
