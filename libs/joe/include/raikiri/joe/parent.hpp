#pragma once

#include <raikiri/joe/order.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::joe {

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
    bool open{true};

    [[nodiscard]] FixedE9 qty() const noexcept { return lot.scaled_by(lots); }

    // a fill past the order's size leaves nothing working rather than a negative
    [[nodiscard]] FixedE9 leaves() const noexcept {
        return !open || filled >= qty() ? FixedE9::from_raw(0) : qty() - filled;
    }
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

    // false when the child is not this parent's
    bool fill(std::uint64_t child_id, FixedE9 qty, FixedE9 price, FixedE9 fee);
    bool close(std::uint64_t child_id);

    [[nodiscard]] bool owns(std::uint64_t child_id) const noexcept;

    void market_trade(FixedE9 qty, FixedE9 price) noexcept;

    // consecutive risk refusals; a submit that goes through clears it
    std::uint32_t refused() noexcept { return ++refusals_; }

    void accepted() noexcept { refusals_ = 0; }

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

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

    [[nodiscard]] std::span<const UnixNanos> times() const noexcept { return times_; }

    [[nodiscard]] std::span<const std::uint64_t> weights() const noexcept { return weights_; }

private:
    std::expected<void, TransitionError> end(UnixNanos now, Status status);

    std::uint64_t id_;
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
    std::vector<UnixNanos> times_;
    std::vector<std::uint64_t> weights_;
};

} // namespace raikiri::joe
