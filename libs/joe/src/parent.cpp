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

std::string_view to_string(ChildEnd end) noexcept {
    switch (end) {
    case ChildEnd::Open:
        return "open";
    case ChildEnd::Filled:
        return "filled";
    case ChildEnd::Cancelled:
        return "cancelled";
    case ChildEnd::Expired:
        return "expired";
    case ChildEnd::Rejected:
        return "rejected";
    case ChildEnd::Denied:
        return "denied";
    case ChildEnd::Lost:
        return "lost";
    }
    return "unknown";
}

Parent::Parent(std::uint64_t id, ParentSpec spec, UnixNanos created) : id_{id}, spec_{spec}, created_{created} {}

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

Child* Parent::find(std::uint64_t child_id) noexcept {
    const auto it = std::ranges::find(children_, child_id, &Child::id);
    return it == children_.end() ? nullptr : &*it;
}

const Child* Parent::child(std::uint64_t child_id) const noexcept {
    const auto it = std::ranges::find(children_, child_id, &Child::id);
    return it == children_.end() ? nullptr : &*it;
}

const Child* Parent::live() const noexcept {
    const auto it = std::ranges::find_if(children_.rbegin(), children_.rend(), &Child::open);
    return it == children_.rend() ? nullptr : &*it;
}

bool Parent::fill(UnixNanos now, std::uint64_t child_id, FixedE9 qty, FixedE9 price, FixedE9 fee, FixedE9 leaves) {
    auto* it = find(child_id);
    if (it == nullptr) {
        return late_fill(now, child_id, qty, price, fee);
    }
    const auto left = std::max<std::int64_t>(it->qty().raw - it->filled.raw, 0);
    const auto dropped = it->open && !leaves.is_undef() ? left - leaves.raw : qty.raw;
    const auto take = FixedE9::from_raw(std::clamp<std::int64_t>(dropped, 0, left));
    if (qty.raw > 0 && take.raw == 0) {
        return true;
    }
    const auto value = value_of(take, price);
    const auto paid = fee.is_undef() ? FixedE9::from_raw(0) : fee;
    it->filled = it->filled + take;
    it->value = it->value + value;
    it->fees = it->fees + paid;
    filled_ = filled_ + take;
    value_ = value_ + value;
    fees_ = fees_ + paid;
    if (it->acked.ns == 0) {
        it->acked = now;
    }
    if (it->open && take.raw > 0 && it->filled >= it->qty()) {
        it->open = false;
        it->closed = now;
        it->end = ChildEnd::Filled;
    }
    trace_.fill(
        {.ts = now, .child_id = child_id, .instrument = it->instrument, .qty = take, .price = price, .fee = paid});
    return true;
}

bool Parent::late_fill(UnixNanos now, std::uint64_t child_id, FixedE9 qty, FixedE9 price, FixedE9 fee) {
    const auto it = std::ranges::find(owed_, child_id, &OwedChild::id);
    if (it == owed_.end()) {
        return false;
    }
    const auto take = FixedE9::from_raw(std::clamp<std::int64_t>(qty.raw, 0, it->left.raw));
    if (take.raw == 0) {
        return true;
    }
    const auto value = value_of(take, price);
    const auto paid = fee.is_undef() ? FixedE9::from_raw(0) : fee;
    auto& row = retired_row(it->instrument.venue());
    row.book(take, value, paid, arrival_, spec_.side == Side::Buy);
    if (!it->any_fill) {
        ++row.sum.filled_children;
        it->any_fill = true;
    }
    it->left = it->left - take;
    filled_ = filled_ + take;
    value_ = value_ + value;
    fees_ = fees_ + paid;
    trace_.fill(
        {.ts = now, .child_id = child_id, .instrument = it->instrument, .qty = take, .price = price, .fee = paid});
    return true;
}

bool Parent::ack(UnixNanos now, std::uint64_t child_id) {
    auto* it = find(child_id);
    if (it == nullptr) {
        return false;
    }
    if (it->acked.ns == 0) {
        it->acked = now;
    }
    return true;
}

bool Parent::close(UnixNanos now, std::uint64_t child_id, ChildEnd end) {
    auto* it = find(child_id);
    if (it == nullptr) {
        return false;
    }
    // a denial never reached the venue and a lost order never answered
    if (it->acked.ns == 0 && end != ChildEnd::Denied && end != ChildEnd::Lost) {
        it->acked = now;
    }
    if (it->open) {
        it->open = false;
        it->closed = now;
        it->end = end;
    }
    return true;
}

void Parent::cancel_sent(UnixNanos now, std::uint64_t child_id) noexcept {
    if (auto* it = find(child_id); it != nullptr) {
        it->cancel_sent = now;
    }
}

void Parent::cancel_refused(std::uint64_t child_id) noexcept {
    if (auto* it = find(child_id); it != nullptr) {
        it->cancel_sent = UnixNanos{};
    }
}

void Parent::modify_sent(UnixNanos now, std::uint64_t child_id, FixedE9 limit) noexcept {
    if (auto* it = find(child_id); it != nullptr) {
        it->modify_sent = now;
        it->modify_limit = limit;
    }
}

bool Parent::modified(UnixNanos now, std::uint64_t child_id) noexcept {
    auto* it = find(child_id);
    if (it == nullptr || it->modify_sent.ns == 0) {
        return false;
    }
    it->modify_sent = UnixNanos{};
    if (!it->open) {
        return false;
    }
    it->limit = it->modify_limit;
    trace_.reprice({.ts = now,
                    .child_id = child_id,
                    .instrument = it->instrument,
                    .price = it->limit,
                    .qty = it->qty(),
                    .why = RepriceWhy::Peg});
    return true;
}

bool Parent::modify_refused(std::uint64_t child_id) noexcept {
    auto* it = find(child_id);
    if (it == nullptr || it->modify_sent.ns == 0) {
        return false;
    }
    it->modify_sent = UnixNanos{};
    return true;
}

bool Parent::owns(std::uint64_t child_id) const noexcept {
    return child(child_id) != nullptr || std::ranges::find(owed_, child_id, &OwedChild::id) != owed_.end();
}

VenueRow& Parent::retired_row(VenueId venue) {
    const auto it = std::ranges::find_if(retired_, [venue](const VenueRow& row) { return row.sum.venue == venue; });
    if (it != retired_.end()) {
        return *it;
    }
    return retired_.emplace_back(VenueRow{.sum = {.venue = venue}});
}

void Parent::retire(UnixNanos now, std::size_t keep, std::uint64_t late_ns) {
    std::erase_if(owed_, [&](const OwedChild& child) { return now.ns - child.closed.ns >= late_ns; });
    if (children_.size() <= keep) {
        return;
    }
    auto excess = children_.size() - keep;
    const auto gone = std::ranges::remove_if(children_, [&](const Child& child) {
        if (excess == 0 || child.open) {
            return false;
        }
        --excess;
        retired_row(child.instrument.venue()).add(child, arrival_, spec_.side == Side::Buy);
        ++retired_count_;
        if (child.filled < child.qty() && child.end != ChildEnd::Denied) {
            owed_.push_back({.id = child.id,
                             .instrument = child.instrument,
                             .left = child.qty() - child.filled,
                             .closed = child.closed,
                             .any_fill = child.filled.raw > 0});
        }
        return true;
    });
    children_.erase(gone.begin(), gone.end());
}

void VenueRow::book(FixedE9 qty, FixedE9 value, FixedE9 fee, FixedE9 arrival, bool buy) noexcept {
    sum.filled = sum.filled + qty;
    sum.value = sum.value + value;
    sum.fees = sum.fees + fee;
    if (qty.raw > 0 && !arrival.is_undef() && arrival.raw > 0) {
        const auto at_arrival = value_of(qty, arrival);
        sum.benchmark = sum.benchmark + at_arrival;
        sum.cost = sum.cost + (buy ? value - at_arrival : at_arrival - value);
    }
}

void VenueRow::ack(std::uint64_t ns) {
    if (acks.size() < k_acks) {
        acks.push_back(ns);
    } else {
        acks[sum.acked % k_acks] = ns;
    }
    ++sum.acked;
    sum.ack_max_ns = std::max(sum.ack_max_ns, ns);
}

void VenueRow::add(const Child& child, FixedE9 arrival, bool buy) {
    ++sum.children;
    sum.sent = sum.sent + child.qty();
    book(child.filled, child.value, child.fees, arrival, buy);
    if (child.filled.raw > 0) {
        ++sum.filled_children;
    }
    if (child.end == ChildEnd::Cancelled || child.end == ChildEnd::Expired) {
        ++sum.cancelled;
    } else if (child.end == ChildEnd::Rejected || child.end == ChildEnd::Denied) {
        ++sum.rejected;
    }
    if (child.acked.ns >= child.sent.ns && child.acked.ns != 0) {
        ack(child.acked.ns - child.sent.ns);
    }
}

void VenueRow::add(const VenueRow& other) {
    sum.children += other.sum.children;
    sum.filled_children += other.sum.filled_children;
    sum.cancelled += other.sum.cancelled;
    sum.rejected += other.sum.rejected;
    sum.sent = sum.sent + other.sum.sent;
    sum.filled = sum.filled + other.sum.filled;
    sum.value = sum.value + other.sum.value;
    sum.fees = sum.fees + other.sum.fees;
    sum.cost = sum.cost + other.sum.cost;
    sum.benchmark = sum.benchmark + other.sum.benchmark;
    for (const auto ns : other.acks) {
        ack(ns);
    }
    sum.ack_max_ns = std::max(sum.ack_max_ns, other.sum.ack_max_ns);
    // acks the other row wrapped past were still acks
    sum.acked += other.sum.acked - static_cast<std::uint32_t>(other.acks.size());
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
