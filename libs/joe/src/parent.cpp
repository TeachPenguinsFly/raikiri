#include <raikiri/joe/parent.hpp>

#include <algorithm>
#include <utility>

namespace raikiri::joe {

std::string_view to_string(TransitionError error) noexcept {
    switch (error) {
    case TransitionError::NotPending:
        return "the parent has already started";
    case TransitionError::NotWorking:
        return "the parent is not working";
    case TransitionError::NotPaused:
        return "the parent is not paused";
    case TransitionError::Terminal:
        return "the parent has finished";
    }
    return "unknown";
}

Parent::Parent(std::uint64_t id, ParentSpec spec, UnixNanos created)
    : id_{id}, spec_{std::move(spec)}, created_{created} {}

std::expected<void, TransitionError> Parent::start(UnixNanos now, FixedE9 qty, FixedE9 arrival) {
    if (status_ != Status::Pending) {
        return std::unexpected{terminal(status_) ? TransitionError::Terminal : TransitionError::NotPending};
    }
    status_ = Status::Working;
    started_ = now;
    qty_ = qty;
    arrival_ = arrival;
    return {};
}

std::expected<void, TransitionError> Parent::pause(UnixNanos) {
    if (status_ != Status::Working) {
        return std::unexpected{terminal(status_) ? TransitionError::Terminal : TransitionError::NotWorking};
    }
    status_ = Status::Paused;
    return {};
}

std::expected<void, TransitionError> Parent::resume(UnixNanos) {
    if (status_ != Status::Paused) {
        return std::unexpected{terminal(status_) ? TransitionError::Terminal : TransitionError::NotPaused};
    }
    status_ = Status::Working;
    return {};
}

std::expected<void, TransitionError> Parent::cancel(UnixNanos now) {
    return end(now, Status::Cancelled);
}

std::expected<void, TransitionError> Parent::finish(UnixNanos now) {
    if (status_ != Status::Working) {
        return std::unexpected{terminal(status_) ? TransitionError::Terminal : TransitionError::NotWorking};
    }
    return end(now, Status::Done);
}

std::expected<void, TransitionError> Parent::fail(UnixNanos now, std::string reason) {
    auto ended = end(now, Status::Failed);
    if (ended.has_value()) {
        reason_ = std::move(reason);
    }
    return ended;
}

std::expected<void, TransitionError> Parent::end(UnixNanos now, Status status) {
    if (terminal(status_)) {
        return std::unexpected{TransitionError::Terminal};
    }
    status_ = status;
    ended_ = now;
    return {};
}

void Parent::plan(std::vector<UnixNanos> times, std::vector<std::uint64_t> weights) {
    times_ = std::move(times);
    weights_ = std::move(weights);
}

void Parent::add(Child child) {
    children_.push_back(child);
}

bool Parent::fill(std::uint64_t child_id, FixedE9 qty, FixedE9 price, FixedE9 fee) {
    const auto it = std::ranges::find(children_, child_id, &Child::id);
    if (it == children_.end()) {
        return false;
    }
    const auto value = value_of(qty, price);
    const auto paid = fee.is_undef() ? FixedE9::from_raw(0) : fee;
    it->filled = it->filled + qty;
    it->value = it->value + value;
    it->fees = it->fees + paid;
    filled_ = filled_ + qty;
    value_ = value_ + value;
    fees_ = fees_ + paid;
    return true;
}

bool Parent::close(std::uint64_t child_id) {
    const auto it = std::ranges::find(children_, child_id, &Child::id);
    if (it == children_.end()) {
        return false;
    }
    it->open = false;
    return true;
}

bool Parent::owns(std::uint64_t child_id) const noexcept {
    return std::ranges::find(children_, child_id, &Child::id) != children_.end();
}

void Parent::market_trade(FixedE9 qty, FixedE9 price) noexcept {
    market_volume_ = market_volume_ + qty;
    market_value_ = market_value_ + value_of(qty, price);
}

FixedE9 Parent::working() const noexcept {
    auto out = FixedE9::from_raw(0);
    for (const auto& child : children_) {
        out = out + child.leaves();
    }
    return out;
}

FixedE9 Parent::remaining() const noexcept {
    const auto left = qty_.raw - filled_.raw - working().raw;
    return FixedE9::from_raw(std::max<std::int64_t>(left, 0));
}

FixedE9 Parent::avg_price() const noexcept {
    if (filled_.raw <= 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(value_.raw, k_e9, filled_.raw));
}

FixedE9 Parent::market_vwap() const noexcept {
    if (market_volume_.raw <= 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(market_value_.raw, k_e9, market_volume_.raw));
}

} // namespace raikiri::joe
