#pragma once

// joe's use cases; single-threaded, driven by the strategy loop that owns the sink

#include <raikiri/common/time.hpp>
#include <raikiri/joe/parent.hpp>
#include <raikiri/joe/ports.hpp>
#include <raikiri/joe/resting.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::joe {

enum class DeskError : std::uint8_t { BadSpec, NotRunning, NoSuchParent, Conflict };

struct Refusal {
    DeskError error{DeskError::BadSpec};
    std::string detail;
};

struct DeskConfig {
    // how long a parent may wait for a venue, and how long past its horizon it may run
    std::uint64_t grace_ns{30ULL * 1'000'000'000ULL};
    // the shortest gap between two batches of children for one parent
    std::uint64_t resend_ns{1'000'000'000ULL};
    std::uint32_t max_refusals{3};
    std::size_t keep{256};
    // the in-flight trace: the shortest mid interval, and how many mids and fills one parent keeps
    std::uint64_t sample_ns{1'000'000'000ULL};
    std::size_t trace_mids{1024};
    std::size_t trace_fills{256};
    std::size_t trace_reprices{1024};
    // an unanswered cancel goes again after this; an unanswered modify turns into a cancel
    std::uint64_t cancel_retry_ns{5ULL * 1'000'000'000ULL};
    std::uint64_t modify_wait_ns{5ULL * 1'000'000'000ULL};
    // resting children the venue turned away in a row before the parent gives up
    std::uint32_t max_rejects{5};
    // finished children one parent lists, and how long strategyd still attributes a late fill
    std::size_t keep_children{256};
    std::uint64_t late_fill_ns{15ULL * 60 * 1'000'000'000ULL};
};

class Desk {
public:
    Desk(const MarketView& market,
         OrderSink& sink,
         const Clock& clock,
         const VolumeProfile& recorded,
         DeskConfig config = {});

    [[nodiscard]] std::expected<std::uint64_t, Refusal> start(ParentSpec spec);
    [[nodiscard]] std::expected<void, Refusal> cancel(std::uint64_t id);
    [[nodiscard]] std::expected<void, Refusal> pause(std::uint64_t id);
    [[nodiscard]] std::expected<void, Refusal> resume(std::uint64_t id);

    [[nodiscard]] const std::deque<Parent>& parents() const noexcept { return parents_; }

    [[nodiscard]] const Parent* find(std::uint64_t id) const noexcept;
    [[nodiscard]] const Parent* find_tag(std::string_view tag) const noexcept;

    [[nodiscard]] UnixNanos now() const noexcept { return clock_->wall(); }

    void tick();

    // events for children; an id no parent owns is ignored
    void on_fill(std::uint64_t child_id,
                 InstrumentId instrument,
                 QtyLots qty,
                 PriceTicks price,
                 FixedE9 fee,
                 QtyLots leaves = QtyLots::undef());
    void on_ack(std::uint64_t child_id);
    void on_closed(std::uint64_t child_id, ChildEnd end);
    void on_modified(std::uint64_t child_id);
    void on_modify_refused(std::uint64_t child_id);
    void on_cancel_refused(std::uint64_t child_id);
    void on_trade(InstrumentId instrument, PriceTicks price, QtyLots qty);

    // the loop stopped: what was working is paused and its children are cancelled
    void halt();

private:
    [[nodiscard]] Parent* find_mut(std::uint64_t id) noexcept;
    void cancel_children(Parent& parent);
    void begin(Parent& parent, UnixNanos now);
    void work(Parent& parent, UnixNanos now);
    void send(Parent& parent, FixedE9 want, std::span<const VenueBook> books, UnixNanos now);
    void rest(Parent& parent, UnixNanos now);
    void place(Parent& parent, const VenueBook& book, FixedE9 price, UnixNanos now);
    void hold(Parent& parent, Hold why, UnixNanos now);
    void pull(Parent& parent, const Child& child, UnixNanos now);
    void chase_cancels(Parent& parent, UnixNanos now);
    [[nodiscard]] Parent* owner_of(std::uint64_t child_id) noexcept;
    void sample(Parent& parent, UnixNanos now);
    void prune();

    const MarketView* market_;
    OrderSink* sink_;
    const Clock* clock_;
    const VolumeProfile* recorded_;
    DeskConfig config_;
    std::deque<Parent> parents_;
    std::uint64_t next_id_{1};
};

} // namespace raikiri::joe
