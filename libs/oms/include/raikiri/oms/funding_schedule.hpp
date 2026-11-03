#pragma once

// when each instrument's funding settles, from the venue's published schedule. a venue charges an account's net, so
// books crossed to exactly zero hear nothing from it; this says when they owe each other

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace raikiri::oms {

struct DueFunding {
    InstrumentId instrument{};
    FixedE9 rate{};
    PriceTicks mark{};
    // the published time, so that a restart books the same settlement under the same identity
    UnixNanos settled_at{};
};

class FundingSchedule {
public:
    void on_update(const FundingUpdate& update);

    // each settlement once; one found later than k_grace_ns is counted in missed(), since today's positions are not
    // what it was owed on
    [[nodiscard]] std::vector<DueFunding> due(UnixNanos now);

    [[nodiscard]] std::uint64_t missed() const noexcept { return missed_; }

private:
    static constexpr std::uint64_t k_grace_ns = 60'000'000'000ULL;

    struct Next {
        DueFunding funding{};
        bool settled{false};
    };

    std::unordered_map<std::uint64_t, Next> next_;
    std::vector<DueFunding> passed_;
    std::uint64_t missed_{0};
};

} // namespace raikiri::oms
