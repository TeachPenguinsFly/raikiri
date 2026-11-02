#pragma once

// what a parent saw while it worked, for the in-flight chart; buffers are sized once at start so
// recording a sample or a fill never allocates, and a full buffer drops its oldest entry

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace raikiri::joe {

struct MidSample {
    std::uint64_t seq{0};
    UnixNanos ts{};
    FixedE9 mid{};
    FixedE9 vwap{};
};

struct FillMark {
    std::uint64_t seq{0};
    UnixNanos ts{};
    std::uint64_t child_id{0};
    InstrumentId instrument{};
    FixedE9 qty{};
    FixedE9 price{};
    FixedE9 fee{};
};

enum class RepriceWhy : std::uint8_t { Place, Refill, Peg, Closed, Rejected, Stale, Crossed };

[[nodiscard]] std::string_view to_string(RepriceWhy why) noexcept;

// the working price from here on; undef once nothing is working
struct RepriceMark {
    std::uint64_t seq{0};
    UnixNanos ts{};
    std::uint64_t child_id{0};
    InstrumentId instrument{};
    FixedE9 price{};
    FixedE9 qty{};
    RepriceWhy why{RepriceWhy::Place};
};

template <typename T> class Ring {
public:
    void open(std::size_t capacity) {
        buf_.assign(capacity, T{});
        pushed_ = 0;
    }

    void push(const T& value) noexcept {
        if (buf_.empty()) {
            return;
        }
        buf_[pushed_ % buf_.size()] = value;
        ++pushed_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return pushed_ < buf_.size() ? pushed_ : buf_.size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

    [[nodiscard]] std::uint64_t dropped() const noexcept { return pushed_ - size(); }

    // oldest first
    [[nodiscard]] const T& at(std::size_t i) const noexcept { return buf_[(pushed_ - size() + i) % buf_.size()]; }

    [[nodiscard]] const T* last() const noexcept { return size() == 0 ? nullptr : &at(size() - 1); }

private:
    std::vector<T> buf_;
    std::uint64_t pushed_{0};
};

class Trace {
public:
    void open(std::uint64_t interval_ns, std::size_t mids, std::size_t fills, std::size_t reprices = 0);

    [[nodiscard]] bool due(UnixNanos now) const noexcept;

    // false when it is sooner than the interval after the last sample
    bool sample(UnixNanos ts, FixedE9 mid, FixedE9 vwap) noexcept;

    void fill(const FillMark& mark) noexcept;

    void reprice(const RepriceMark& mark) noexcept;

    // the newest entry's seq; a reader that passes it back as `since` gets only what is newer
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

    [[nodiscard]] std::uint64_t interval_ns() const noexcept { return interval_ns_; }

    [[nodiscard]] const Ring<MidSample>& mids() const noexcept { return mids_; }

    [[nodiscard]] const Ring<FillMark>& fills() const noexcept { return fills_; }

    [[nodiscard]] const Ring<RepriceMark>& reprices() const noexcept { return reprices_; }

private:
    std::uint64_t interval_ns_{0};
    std::uint64_t seq_{0};
    Ring<MidSample> mids_;
    Ring<FillMark> fills_;
    Ring<RepriceMark> reprices_;
};

// the mid interval and buffer size that cover the whole run within `max_mids` samples
struct TraceShape {
    std::uint64_t interval_ns{0};
    std::size_t mids{0};
};

[[nodiscard]] TraceShape
trace_shape(std::uint64_t run_ns, std::uint64_t min_interval_ns, std::size_t max_mids) noexcept;

} // namespace raikiri::joe
