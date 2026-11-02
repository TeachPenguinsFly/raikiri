#pragma once

#include <raikiri/joe/order.hpp>
#include <raikiri/joe/trace.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::joe {

enum class ChildEnd : std::uint8_t { Open, Filled, Cancelled, Expired, Rejected, Denied, Lost };

[[nodiscard]] std::string_view to_string(ChildEnd end) noexcept;

struct Child {
    std::uint64_t id{0};
    InstrumentId instrument{};
    FixedE9 lot{};
    std::int64_t lots{0};
    FixedE9 limit{};
    FixedE9 filled{FixedE9::from_raw(0)};
    FixedE9 value{FixedE9::from_raw(0)};
    FixedE9 fees{FixedE9::from_raw(0)};
    UnixNanos sent{};
    // the venue's first answer of any kind, so an ioc that fills without an ack still has one
    UnixNanos acked{};
    UnixNanos closed{};
    ChildEnd end{ChildEnd::Open};
    bool open{true};
    ChildTif tif{ChildTif::Ioc};
    // an answer is owed while these are set: a cancel holds the slot, a modify holds `modify_limit`
    UnixNanos cancel_sent{};
    UnixNanos modify_sent{};
    FixedE9 modify_limit{};

    [[nodiscard]] FixedE9 qty() const noexcept { return lot.scaled_by(lots); }

    // a fill past the order's size leaves nothing working rather than a negative
    [[nodiscard]] FixedE9 leaves() const noexcept {
        return !open || filled >= qty() ? FixedE9::from_raw(0) : qty() - filled;
    }
};

// one venue's child orders summed
struct VenueTca {
    VenueId venue{VenueId::Invalid};
    std::uint32_t children{0};
    std::uint32_t filled_children{0};
    std::uint32_t cancelled{0};
    std::uint32_t rejected{0};
    std::uint32_t acked{0};
    FixedE9 sent{FixedE9::from_raw(0)};
    FixedE9 filled{FixedE9::from_raw(0)};
    FixedE9 value{FixedE9::from_raw(0)};
    FixedE9 fees{FixedE9::from_raw(0)};
    // positive is a cost: what the fills paid beyond the arrival mid, and that mid's worth of them
    FixedE9 cost{FixedE9::from_raw(0)};
    FixedE9 benchmark{FixedE9::from_raw(0)};
    std::uint64_t ack_p50_ns{0};
    std::uint64_t ack_p90_ns{0};
    std::uint64_t ack_max_ns{0};
};

// the sum and the newest acks behind its percentiles
struct VenueRow {
    static constexpr std::size_t k_acks = 4096;

    VenueTca sum{};
    std::vector<std::uint64_t> acks{};

    void add(const Child& child, FixedE9 arrival, bool buy);
    void add(const VenueRow& other);
    void book(FixedE9 qty, FixedE9 value, FixedE9 fee, FixedE9 arrival, bool buy) noexcept;
    void ack(std::uint64_t ns);
};

// a finished child no longer held whose venue may still report a fill it owes
struct OwedChild {
    std::uint64_t id{0};
    InstrumentId instrument{};
    FixedE9 left{};
    UnixNanos closed{};
    bool any_fill{false};
};

enum class Hold : std::uint8_t { None, Stale, Crossed };

// a resting algo's one slot: at most one child is live in it
struct Slot {
    std::uint64_t random{0};
    UnixNanos changed{};
    UnixNanos retry_at{};
    std::uint32_t rejects{0};
    Hold hold{Hold::None};
    bool no_modify{false};
};

enum class TransitionError : std::uint8_t { NotPending, NotWorking, NotPaused, Terminal };

[[nodiscard]] std::string_view to_string(TransitionError error) noexcept;

// pending -> working -> done | cancelled | failed, working <-> paused; a fill is booked in any
// state, so one that lands after a cancel still counts
class Parent {
public:
    Parent(std::uint64_t id, ParentSpec spec, UnixNanos created);

    std::expected<void, TransitionError> start(UnixNanos now, FixedE9 qty, FixedE9 arrival);
    std::expected<void, TransitionError> pause(UnixNanos now);
    std::expected<void, TransitionError> resume(UnixNanos now);
    std::expected<void, TransitionError> cancel(UnixNanos now);
    std::expected<void, TransitionError> finish(UnixNanos now);
    std::expected<void, TransitionError> fail(UnixNanos now, std::string reason);

    void plan(std::vector<UnixNanos> times, std::vector<std::uint64_t> weights);

    void add(Child child);

    // false when the child is not this parent's. a child books at most its own size, the way
    // strategyd books the position: with `leaves` the drop to it, without them or once closed the
    // fill capped at what was left; a child that reaches its size closes as filled
    bool fill(UnixNanos now,
              std::uint64_t child_id,
              FixedE9 qty,
              FixedE9 price,
              FixedE9 fee,
              FixedE9 leaves = FixedE9::undef());
    bool ack(UnixNanos now, std::uint64_t child_id);
    bool close(UnixNanos now, std::uint64_t child_id, ChildEnd end);

    void cancel_sent(UnixNanos now, std::uint64_t child_id) noexcept;
    void cancel_refused(std::uint64_t child_id) noexcept;
    void modify_sent(UnixNanos now, std::uint64_t child_id, FixedE9 limit) noexcept;
    // the modify's answer; false when none was owed
    bool modified(UnixNanos now, std::uint64_t child_id) noexcept;
    bool modify_refused(std::uint64_t child_id) noexcept;

    // the oldest finished children past `keep` leave the list for the venue totals; one that left
    // size behind stays answerable for a late fill until `late_ns` after it closed
    void retire(UnixNanos now, std::size_t keep, std::uint64_t late_ns);

    [[nodiscard]] bool owns(std::uint64_t child_id) const noexcept;

    void market_trade(FixedE9 qty, FixedE9 price) noexcept;

    // consecutive risk refusals; a submit that goes through clears it
    std::uint32_t refused() noexcept { return ++refusals_; }

    void accepted() noexcept { refusals_ = 0; }

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    // moves with every change and never back, a restart included
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

    [[nodiscard]] const ParentSpec& spec() const noexcept { return spec_; }

    [[nodiscard]] Status status() const noexcept { return status_; }

    [[nodiscard]] std::string_view reason() const noexcept { return reason_; }

    [[nodiscard]] UnixNanos created() const noexcept { return created_; }

    [[nodiscard]] UnixNanos started() const noexcept { return started_; }

    [[nodiscard]] UnixNanos ended() const noexcept { return ended_; }

    [[nodiscard]] FixedE9 qty() const noexcept { return qty_; }

    [[nodiscard]] FixedE9 arrival() const noexcept { return arrival_; }

    [[nodiscard]] FixedE9 filled() const noexcept { return filled_; }

    [[nodiscard]] FixedE9 value() const noexcept { return value_; }

    [[nodiscard]] FixedE9 fees() const noexcept { return fees_; }

    [[nodiscard]] FixedE9 working() const noexcept;
    // what is neither filled nor working; never negative, so an overfill reads as zero
    [[nodiscard]] FixedE9 remaining() const noexcept;
    [[nodiscard]] FixedE9 avg_price() const noexcept;

    [[nodiscard]] FixedE9 market_volume() const noexcept { return market_volume_; }

    [[nodiscard]] FixedE9 market_vwap() const noexcept;

    [[nodiscard]] std::span<const Child> children() const noexcept { return children_; }

    // the newest open child; a resting algo never has two
    [[nodiscard]] const Child* live() const noexcept;

    [[nodiscard]] const Child* child(std::uint64_t child_id) const noexcept;

    // every child ever sent, held or retired
    [[nodiscard]] std::uint64_t child_count() const noexcept { return retired_count_ + children_.size(); }

    [[nodiscard]] std::uint64_t retired_count() const noexcept { return retired_count_; }

    [[nodiscard]] std::span<const VenueRow> retired() const noexcept { return retired_; }

    [[nodiscard]] Slot& slot() noexcept { return slot_; }

    [[nodiscard]] const Slot& slot() const noexcept { return slot_; }

    [[nodiscard]] std::span<const UnixNanos> times() const noexcept { return times_; }

    [[nodiscard]] std::span<const std::uint64_t> weights() const noexcept { return weights_; }

    [[nodiscard]] Trace& trace() noexcept { return trace_; }

    [[nodiscard]] const Trace& trace() const noexcept { return trace_; }

private:
    std::expected<void, TransitionError> end(UnixNanos now, Status status);

    void touch(UnixNanos now) noexcept { version_ = std::max(version_ + 1, now.ns); }

    [[nodiscard]] Child* find(std::uint64_t child_id) noexcept;
    bool late_fill(UnixNanos now, std::uint64_t child_id, FixedE9 qty, FixedE9 price, FixedE9 fee);
    [[nodiscard]] VenueRow& retired_row(VenueId venue);

    std::uint64_t id_;
    std::uint64_t version_{0};
    ParentSpec spec_;
    Status status_{Status::Pending};
    std::string reason_;
    UnixNanos created_;
    UnixNanos started_{};
    UnixNanos ended_{};
    FixedE9 qty_{FixedE9::from_raw(0)};
    FixedE9 arrival_{FixedE9::undef()};
    FixedE9 filled_{FixedE9::from_raw(0)};
    FixedE9 value_{FixedE9::from_raw(0)};
    FixedE9 fees_{FixedE9::from_raw(0)};
    FixedE9 market_volume_{FixedE9::from_raw(0)};
    FixedE9 market_value_{FixedE9::from_raw(0)};
    std::uint32_t refusals_{0};
    std::vector<Child> children_;
    std::vector<OwedChild> owed_;
    std::vector<VenueRow> retired_;
    std::uint64_t retired_count_{0};
    Slot slot_{};
    std::vector<UnixNanos> times_;
    std::vector<std::uint64_t> weights_;
    Trace trace_;
};

} // namespace raikiri::joe
