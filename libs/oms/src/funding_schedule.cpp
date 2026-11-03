#include <raikiri/oms/funding_schedule.hpp>

#include <utility>

namespace raikiri::oms {

void FundingSchedule::on_update(const FundingUpdate& update) {
    // continuous funding has no instant to settle at
    if (update.accrual == FundingAccrual::Continuous || update.next_funding_time.ns == 0 || update.rate.is_undef()) {
        return;
    }
    auto mark = update.mark_price;
    if (mark.is_undef()) {
        mark = update.index_price;
    }
    auto& held = next_[update.instrument.packed];
    // the venue moved on before the clock here got there
    if (held.funding.settled_at.ns != 0 && update.next_funding_time.ns > held.funding.settled_at.ns && !held.settled) {
        passed_.push_back(held.funding);
    }
    if (update.next_funding_time.ns != held.funding.settled_at.ns) {
        held.settled = false;
    }
    held.funding = {
        .instrument = update.instrument, .rate = update.rate, .mark = mark, .settled_at = update.next_funding_time};
}

std::vector<DueFunding> FundingSchedule::due(UnixNanos now) {
    auto out = std::exchange(passed_, {});
    for (auto& [packed, held] : next_) {
        if (!held.settled && held.funding.settled_at.ns <= now.ns) {
            held.settled = true;
            out.push_back(held.funding);
        }
    }
    std::erase_if(out, [&](const DueFunding& funding) {
        if (funding.settled_at.ns + k_grace_ns >= now.ns) {
            return false;
        }
        ++missed_;
        return true;
    });
    return out;
}

} // namespace raikiri::oms
