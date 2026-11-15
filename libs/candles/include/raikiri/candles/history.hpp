#pragma once

#include <raikiri/candles/candle.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::candles {

// one public REST call for a stretch of one-minute bars
struct KlineCall {
    std::string host;
    std::string target;
    std::string body{}; // a POST when not empty
};

// a venue's one-minute history over its public REST api, in the units our feed records: prices decimal e9 and the
// quantity in the instrument's own units, so a venue minute lines up with ours
class KlineVenue {
public:
    KlineVenue() = default;
    KlineVenue(const KlineVenue&) = delete;
    KlineVenue& operator=(const KlineVenue&) = delete;
    virtual ~KlineVenue() = default;

    // minutes [from, to), at most per_call() of them
    [[nodiscard]] virtual KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const = 0;
    [[nodiscard]] virtual std::int32_t per_call() const noexcept = 0;
    // the calls a minute a backfill may spend, well under the venue's public limit since the gateway shares the ip
    [[nodiscard]] virtual std::uint32_t per_minute() const noexcept = 0;
    // every closed minute in the answer, oldest first
    [[nodiscard]] virtual std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                                                InstrumentId instrument) const = 0;
};

// null where the venue's history is not read yet
[[nodiscard]] std::unique_ptr<KlineVenue> kline_venue(VenueId venue, MarketType market);

// the stretches our feed missed, asked of each venue in turn at its own pace, the busiest markets first
class Backfill {
public:
    struct Gap {
        InstrumentId instrument{};
        std::string symbol; // the venue's own
        std::int32_t from{0};
        std::int32_t to{0};
    };

    // a venue without history support is skipped; each gap is cut into calls of the venue's size
    void add(const Gap& gap);

    // one call for each venue whose pace allows it at `mono_ns`; a stretch reaching past the last closed minute waits
    struct Due {
        Gap part;
        KlineCall call;
        const KlineVenue* source{nullptr};
    };

    [[nodiscard]] std::vector<Due> due(std::uint64_t mono_ns, std::int32_t now_minute);
    // a call that failed goes back to the front once, then is dropped
    void retry(const Due& due);
    // a call that never left (no free connection) goes back to the front without spending a try
    void defer(const Due& due);

    [[nodiscard]] std::size_t queued() const noexcept;

    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

private:
    struct Lane {
        std::unique_ptr<KlineVenue> source;
        std::deque<Gap> parts;
        std::uint64_t next_ns{0};
        std::unordered_map<std::uint64_t, int> tries;
    };

    std::unordered_map<std::uint16_t, Lane> lanes_; // by venue and market
    std::uint64_t dropped_{0};
};

} // namespace raikiri::candles
