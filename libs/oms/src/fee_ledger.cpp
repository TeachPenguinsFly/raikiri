#include <raikiri/oms/fee_ledger.hpp>

#include <algorithm>

namespace raikiri::oms {

void FeeLedger::book(std::string_view reference, FixedE9 amount, UnixNanos at) {
    if (reference.empty() || amount.raw == 0) {
        return; // nothing to quote back, and nothing to disagree about
    }
    booked_.insert_or_assign(std::string{reference}, Memo{.amount = amount, .booked_at = at});
}

FeeCheck FeeLedger::reconcile(const LedgerEntry& charge) const {
    const auto it = booked_.find(std::string{charge.reference.view()});
    if (it == booked_.end()) {
        return {};
    }
    return {.verdict = it->second.amount.raw == charge.amount.raw ? FeeVerdict::Agreed
                                                                  : FeeVerdict::Disagreed,
            .booked = it->second.amount};
}

void FeeLedger::prune(UnixNanos now, std::uint64_t max_age_ns) {
    std::erase_if(booked_, [&](const auto& entry) {
        return now.ns > entry.second.booked_at.ns &&
               now.ns - entry.second.booked_at.ns > max_age_ns;
    });
}

} // namespace raikiri::oms
