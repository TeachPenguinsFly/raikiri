#pragma once

// at most `max` cost inside any window of `window_ns`

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raikiri {

class SlidingLog {
public:
    SlidingLog() = default;

    SlidingLog(std::uint32_t max, std::uint64_t window_ns, std::uint32_t reserve = 0)
        : ring_(max), max_{max}, window_ns_{window_ns}, reserve_{std::min(reserve, max)} {}

    // zero when `cost` may be spent now, else the wait; only a reserve-allowed caller spends the
    // last `reserve` units
    [[nodiscard]] std::uint64_t
    wait_ns(std::uint32_t cost, std::uint64_t now_ns, bool may_use_reserve = true) noexcept {
        if (now_ns < held_until_) {
            return held_until_ - now_ns;
        }
        expire(now_ns);
        const std::uint64_t ceiling = may_use_reserve ? max_ : max_ - reserve_;
        if (used_ + cost <= ceiling) {
            return 0;
        }
        if (cost > ceiling) {
            return window_ns_;
        }
        const std::uint64_t need = used_ + cost - ceiling;
        std::uint64_t freed = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& entry = ring_[(head_ + i) % ring_.size()];
            freed += entry.cost;
            if (freed >= need) {
                return entry.at + window_ns_ - now_ns;
            }
        }
        return window_ns_;
    }

    void spend(std::uint32_t cost, std::uint64_t now_ns) noexcept {
        if (cost == 0 || ring_.empty()) {
            return;
        }
        expire(now_ns);
        if (size_ == ring_.size()) {
            pop();
        }
        ring_[(head_ + size_) % ring_.size()] = Entry{.at = now_ns, .cost = cost};
        ++size_;
        used_ += cost;
    }

    void hold(std::uint64_t now_ns, std::uint64_t backoff_ns) noexcept {
        held_until_ = std::max(held_until_, now_ns + backoff_ns);
    }

    [[nodiscard]] std::uint32_t max() const noexcept { return max_; }

    [[nodiscard]] std::uint64_t window_ns() const noexcept { return window_ns_; }

private:
    struct Entry {
        std::uint64_t at{0};
        std::uint32_t cost{0};
    };

    void expire(std::uint64_t now_ns) noexcept {
        while (size_ != 0 && ring_[head_].at + window_ns_ <= now_ns) {
            pop();
        }
    }

    void pop() noexcept {
        used_ -= ring_[head_].cost;
        head_ = (head_ + 1) % ring_.size();
        --size_;
    }

    std::vector<Entry> ring_;
    std::size_t head_{0};
    std::size_t size_{0};
    std::uint64_t used_{0};
    std::uint32_t max_{0};
    std::uint64_t window_ns_{0};
    std::uint32_t reserve_{0};
    std::uint64_t held_until_{0};
};

} // namespace raikiri
