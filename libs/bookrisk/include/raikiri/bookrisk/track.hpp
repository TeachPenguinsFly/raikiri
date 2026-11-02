#pragma once

// a book's moving counts; fixed size, so the order path never allocates here, and driven only by the stamps it is
// handed, so a replay counts the same

#include <raikiri/bookrisk/limits.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/oms/risk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raikiri::bookrisk {

class WindowCount {
public:
    void push(std::uint64_t at_ns) noexcept {
        at_[next_] = at_ns;
        next_ = (next_ + 1) % at_.size();
        used_ = used_ < at_.size() ? used_ + 1 : used_;
    }

    // how many of the stamps fall in the `window_ns` up to `now_ns`, at most k_window_capacity
    [[nodiscard]] std::uint32_t count(std::uint64_t now_ns, std::uint64_t window_ns) const noexcept {
        const auto from = now_ns > window_ns ? now_ns - window_ns : 0;
        std::uint32_t out = 0;
        for (std::size_t back = 1; back <= used_; ++back) {
            if (at_[(next_ + at_.size() - back) % at_.size()] <= from) {
                break;
            }
            ++out;
        }
        return out;
    }

    void clear() noexcept {
        used_ = 0;
        next_ = 0;
    }

private:
    std::array<std::uint64_t, k_window_capacity> at_{};
    std::size_t next_{0};
    std::size_t used_{0};
};

class BookTrack {
public:
    void on_fill(InstrumentId instrument) noexcept;

    // what the gate reads about this book and this order
    void fill(const OrderCmd& cmd, const oms::RiskConfig& config, oms::Exposure& exposure) const noexcept;

    // an order went out: every create and amend counts against the rate; a create that adds risk after a fill on its
    // instrument is one re-entry
    void on_sent(const OrderCmd& cmd, const oms::Exposure& exposure) noexcept;

    // a person re-enabled the book; the cycles that stopped it are not held against it again
    void forgive() noexcept;

private:
    [[nodiscard]] bool filled_since_entry(InstrumentId instrument) const noexcept;

    WindowCount orders_{};
    WindowCount repeats_{};
    // instruments filled since their last entry, oldest overwritten: 64 at once is past any book worth tracking, and
    // losing one only misses a cycle
    std::array<std::uint64_t, 64> filled_{};
    std::size_t next_filled_{0};
};

struct LossChange {
    FixedE9 change{FixedE9::from_raw(0)};
    // how far back the samples reached, short of the window after a start or a gap
    std::uint64_t covered_ns{0};
};

// realised plus unrealised against itself over the last hour, sampled by the caller at a steady pace
class LossSpeed {
public:
    static constexpr std::size_t k_samples = 3'601;

    LossSpeed() : samples_(k_samples) {}

    void sample(UnixNanos at, FixedE9 pnl) noexcept;

    [[nodiscard]] LossChange change(UnixNanos now, std::uint64_t window_ns) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return used_; }

private:
    struct Sample {
        std::uint64_t at_ns{0};
        FixedE9 pnl{FixedE9::from_raw(0)};
    };

    [[nodiscard]] const Sample& nth_oldest(std::size_t n) const noexcept {
        const auto oldest = used_ < samples_.size() ? std::size_t{0} : next_;
        return samples_[(oldest + n) % samples_.size()];
    }

    std::vector<Sample> samples_;
    std::size_t next_{0};
    std::size_t used_{0};
};

} // namespace raikiri::bookrisk
