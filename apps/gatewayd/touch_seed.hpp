#pragma once

#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace raikiri::gatewayd {

// a top-of-book stream is silent on a top that does not move, so a quiet market has no price after a (re)subscribe:
// gate spot left 157 of 2085 pairs unpriced an hour after a restart (2026-11-27). One rest read of the touch fills
// the markets the socket has not quoted; the socket stays the source and overwrites it on its first quote
class TouchSeed {
public:
    static constexpr std::uint64_t k_delay_ns = 20'000'000'000ULL;
    static constexpr std::uint64_t k_retry_ns = 2'000'000'000ULL;

    explicit TouchSeed(std::vector<InstrumentId> subscribed) noexcept : subscribed_{std::move(subscribed)} {}

    // after the subscriptions and after every socket reopen
    void arm(std::uint64_t now_ns) noexcept { due_ns_ = now_ns + k_delay_ns; }

    // a refused read asks again soon rather than at the next reopen
    void retry(std::uint64_t now_ns) noexcept { due_ns_ = now_ns + k_retry_ns; }

    // the pools' reopen count; a rise arms a pass
    void reopens(std::uint64_t count, std::uint64_t now_ns) noexcept {
        if (count > reopens_) {
            arm(now_ns);
        }
        reopens_ = count;
    }

    // the subscribed markets `priced` says the socket has not quoted, once a pass is due; each pass is taken once
    template <typename Priced> [[nodiscard]] std::vector<InstrumentId> take(std::uint64_t now_ns, Priced&& priced) {
        std::vector<InstrumentId> out;
        if (due_ns_ == 0 || now_ns < due_ns_) {
            return out;
        }
        due_ns_ = 0;
        for (const auto id : subscribed_) {
            if (!priced(id)) {
                out.push_back(id);
            }
        }
        return out;
    }

private:
    std::vector<InstrumentId> subscribed_;
    std::uint64_t due_ns_{0};
    std::uint64_t reopens_{0};
};

// a seed lands only in a cell the socket has never quoted, however late its reply comes
[[nodiscard]] constexpr bool keeps_seed(std::uint64_t socket_quoted_ns) noexcept {
    return socket_quoted_ns == 0;
}

} // namespace raikiri::gatewayd
