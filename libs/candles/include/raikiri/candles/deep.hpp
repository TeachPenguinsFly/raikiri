#pragma once

#include <raikiri/candles/funding.hpp>
#include <raikiri/candles/history.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace raikiri::candles {

// weeks of minutes for the busiest markets, asked of each venue only while its candle and funding backfills are idle:
// each instrument walks back from its oldest stored minute in the venue's largest pages, the busiest first
class DeepBackfill {
public:
    // one day short of the floor is deep enough: a restart's hole is the normal backfill's, not ours
    static constexpr std::int32_t k_slack = 24 * 60;

    // the gap is [floor, oldest stored minute); false for a venue without history and for a gap within the slack
    bool add(const Backfill::Gap& gap);

    using Due = Backfill::Due;

    [[nodiscard]] std::vector<Due> due(std::uint64_t mono_ns, const Backfill& klines, const FundingBackfill& funding);
    // an answer without a minute in its window means the venue keeps nothing older: the instrument stops there
    void answered(const Due& due, std::size_t minutes);
    // a call that failed goes back to the front once, then is dropped
    void retry(const Due& due);
    void defer(const Due& due);

    // what a venue and market has queued and the pace it is asked at
    struct Load {
        VenueId venue{};
        MarketType market{};
        std::size_t calls{0};
        std::uint32_t per_minute{0};
    };

    [[nodiscard]] std::vector<Load> load() const;
    [[nodiscard]] std::size_t queued() const noexcept;

    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

private:
    struct Lane {
        std::unique_ptr<KlineVenue> source;
        std::deque<Backfill::Gap> parts;
        std::uint64_t next_ns{0};
        std::unordered_map<std::uint64_t, int> tries;
    };

    std::unordered_map<std::uint16_t, Lane> lanes_; // by venue and market
    std::uint64_t dropped_{0};
};

} // namespace raikiri::candles
