#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace raikiri::candles {

// where a candle's prints came from: our own feed, or the venue's history filling a gap in it
enum class Source : std::int16_t { Live = 0, Venue = 1 };

// one minute of one instrument's prints; prices and quantity are decimal e9, the quantity in the instrument's own
// units (contracts where it trades in contracts)
struct Candle {
    InstrumentId instrument{};
    std::int32_t minute{0}; // unix minutes
    std::int64_t open{0};
    std::int64_t high{0};
    std::int64_t low{0};
    std::int64_t close{0};
    std::int64_t volume{0};
    std::int32_t trades{0};
    Source source{Source::Live};
};

[[nodiscard]] constexpr std::int32_t minute_of(UnixNanos ts) noexcept {
    return static_cast<std::int32_t>(ts.ns / 60'000'000'000ULL);
}

// folds prints into one open minute per instrument and hands back each minute once it has ended
class MinuteBuilder {
public:
    // a print later than its instrument's open minute closes that minute first; one earlier than it is counted and
    // dropped, since its minute was already handed back
    void trade(InstrumentId instrument, std::int64_t price, std::int64_t qty, UnixNanos ts);
    // closes every open minute that ended at least `grace_ns` before `now`
    void close_through(UnixNanos now, std::uint64_t grace_ns);
    // closes every open minute, for a clean stop; the last one may be partial
    void close_all();
    // the minutes closed since the last take, oldest first per instrument
    [[nodiscard]] std::vector<Candle> take();

    [[nodiscard]] std::size_t open_minutes() const noexcept { return open_.size(); }

    [[nodiscard]] std::uint64_t late() const noexcept { return late_; }

    [[nodiscard]] std::uint64_t prints() const noexcept { return prints_; }

private:
    std::unordered_map<std::uint64_t, Candle> open_;
    std::vector<Candle> closed_;
    std::uint64_t late_{0};
    std::uint64_t prints_{0};
};

} // namespace raikiri::candles
