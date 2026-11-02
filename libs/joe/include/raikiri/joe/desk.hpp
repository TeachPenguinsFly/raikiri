#pragma once

// joe's use cases; single-threaded, driven by the strategy loop that owns the sink

#include <raikiri/common/time.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/joe/parent.hpp>
#include <raikiri/joe/ports.hpp>
#include <raikiri/joe/resting.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
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
    // a journal row names its parent by this number, so a restart must start past every earlier one
    std::uint64_t first_parent_id{1};
    // the book of a parent that names none
    std::uint16_t own_book{0};
};

// past any id an earlier process handed out, with room for 4096 parents a millisecond
[[nodiscard]] constexpr std::uint64_t parent_id_seed(UnixNanos wall) noexcept {
    return (wall.ns / 1'000'000ULL) << 12U;
}

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

    // a restart reads these back; null keeps nothing, for a desk nobody restarts
    void set_log(ParentLog* log) noexcept { log_ = log; }

    // the journal's frames in order, all before the first tick. nothing here reaches the sink: what was out is out,
    // and its answers come through these or through the live path
    void restore(const ParentRecord& record);
    void restore(const OrderEvent& event);
    // what was working waits, paused, until the venue has agreed with the book; a child may still be out
    void finish_restore();
    // true for each parent restored working and now working again
    std::size_t resume_restored();

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
    void open_trace(Parent& parent);
    void log_child(const Parent& parent, const Child& child);
    void log_changes();

    // a status moves in many places; each entry point writes what moved on its way out
    class LogChanges {
    public:
        explicit LogChanges(Desk& desk) noexcept : desk_{&desk} {}

        ~LogChanges() { desk_->log_changes(); }

        LogChanges(const LogChanges&) = delete;
        LogChanges& operator=(const LogChanges&) = delete;

    private:
        Desk* desk_;
    };

    const MarketView* market_;
    OrderSink* sink_;
    const Clock* clock_;
    const VolumeProfile* recorded_;
    DeskConfig config_;
    std::deque<Parent> parents_;
    std::uint64_t next_id_;
    ParentLog* log_{nullptr};
    bool halting_{false};
    std::unordered_map<std::uint64_t, Status> logged_;
    std::vector<std::uint64_t> restart_paused_;
};

} // namespace raikiri::joe
