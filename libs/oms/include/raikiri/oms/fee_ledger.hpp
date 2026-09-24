#pragma once

// booked fees awaiting the venue's quote; the verdict is agreed, disagreed or unmatched

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
    // undef when unmatched
    FixedE9 booked{};
};

class FeeLedger {
public:
    // keyed on the venue's execution id, which its ledger quotes back
    void book(std::string_view reference, FixedE9 amount, UnixNanos at);

    [[nodiscard]] FeeCheck reconcile(const LedgerEntry& charge) const;

    // drops entries older than the venue's ledger window
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
