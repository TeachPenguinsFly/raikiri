#include <raikiri/oms/repeat_guard.hpp>

#include <algorithm>

namespace raikiri::oms {

RepeatGuard::RepeatGuard(std::size_t slots) : sent_(std::max<std::size_t>(slots, 1)) {}

std::optional<ClientOrderId>
RepeatGuard::held(const OrderShape& shape, UnixNanos now, std::uint64_t window_ns) const noexcept {
    if (window_ns == 0) {
        return std::nullopt;
    }
    // newest first, and only as far back as the window
    for (std::size_t back = 1; back <= sent_.size(); ++back) {
        const auto& one = sent_[(next_ + sent_.size() - back) % sent_.size()];
        if (one.at_ns == 0 || now.ns - one.at_ns >= window_ns) {
            return std::nullopt;
        }
        if (!one.withdrawn && one.shape == shape) {
            return one.id;
        }
    }
    return std::nullopt;
}

void RepeatGuard::sent(const OrderShape& shape, ClientOrderId id, UnixNanos now) noexcept {
    sent_[next_] = {.shape = shape, .id = id, .at_ns = now.ns, .withdrawn = false};
    next_ = (next_ + 1) % sent_.size();
}

void RepeatGuard::withdrawn(ClientOrderId id) noexcept {
    for (auto& one : sent_) {
        if (one.id == id) {
            one.withdrawn = true;
        }
    }
}

} // namespace raikiri::oms
