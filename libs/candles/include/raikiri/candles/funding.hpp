#pragma once

#include <raikiri/candles/history.hpp>
#include <raikiri/candles/metrics.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::candles {

// a perp's settled funding over a venue's public REST api: one sample per funding time, at its minute, the rate alone
class FundingVenue {
public:
    // how the rest of a window is asked once an answer comes back full
    enum class More : std::uint8_t {
        None,  // one call holds all the venue gives
        Older, // the answer is the newest rows of the window
        Newer, // the answer is the oldest rows of the window
        Page,  // the answer is a numbered page, the newest first
    };

    FundingVenue() = default;
    FundingVenue(const FundingVenue&) = delete;
    FundingVenue& operator=(const FundingVenue&) = delete;
    virtual ~FundingVenue() = default;

    // funding times in the minutes [from, to); pages count from 1
    [[nodiscard]] virtual KlineCall
    call(std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t page) const = 0;
    // the rows a full answer holds
    [[nodiscard]] virtual std::size_t per_call() const noexcept = 0;
    [[nodiscard]] virtual More more() const noexcept = 0;
    // the calls a minute the backfill may spend once the venue's candle history is done
    [[nodiscard]] virtual std::uint32_t per_minute() const noexcept = 0;
    // every row of the answer, in the venue's order
    [[nodiscard]] virtual std::expected<std::vector<MetricSample>, std::string>
    parse(std::string_view body, InstrumentId instrument) const = 0;
};

// null where the venue's funding history is not read yet
[[nodiscard]] std::unique_ptr<FundingVenue> funding_venue(VenueId venue, MarketType market);

// past funding of each instrument, asked once, each venue at its own pace and only while its candle backfill is idle
class FundingBackfill {
public:
    struct Part {
        InstrumentId instrument{};
        std::string symbol; // the venue's own
        std::int32_t from{0};
        std::int32_t to{0};
        std::int32_t page{1};
        int tries{0};
    };

    // false for a venue without funding history
    bool add(const Part& part);

    struct Due {
        Part part;
        KlineCall call;
        const FundingVenue* source{nullptr};
    };

    [[nodiscard]] std::vector<Due> due(std::uint64_t mono_ns, const Backfill& klines);
    // the rows an answer held, as parsed: a full one queues the rest of its window ahead of the next instrument
    void answered(const Due& due, std::span<const MetricSample> rows);
    // a call that failed goes back to the front once, then is dropped
    void retry(const Due& due);
    void defer(const Due& due);

    [[nodiscard]] std::size_t queued() const noexcept;
    // whether calls still wait on the instrument's venue and market
    [[nodiscard]] bool busy(InstrumentId instrument) const noexcept;

    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

private:
    struct Lane {
        std::unique_ptr<FundingVenue> source;
        std::deque<Part> parts;
        std::uint64_t next_ns{0};
    };

    std::unordered_map<std::uint16_t, Lane> lanes_; // by venue and market
    std::uint64_t dropped_{0};
};

} // namespace raikiri::candles
