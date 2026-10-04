#pragma once

// joe's use cases; single-threaded, driven by the strategy loop that owns the sink

#include <raikiri/common/time.hpp>
#include <raikiri/joe/parent.hpp>
#include <raikiri/joe/ports.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <string>
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

    [[nodiscard]] UnixNanos now() const noexcept { return clock_->wall(); }

    void tick();

    // events for children; an id no parent owns is ignored
    void on_fill(std::uint64_t child_id, InstrumentId instrument, QtyLots qty, PriceTicks price, FixedE9 fee);
    void on_closed(std::uint64_t child_id);
    void on_trade(InstrumentId instrument, PriceTicks price, QtyLots qty);

    // the loop stopped: what was working is paused and its children are cancelled
    void halt();

private:
    [[nodiscard]] Parent* find_mut(std::uint64_t id) noexcept;
    void cancel_children(Parent& parent);
    void begin(Parent& parent, UnixNanos now);
    void work(Parent& parent, UnixNanos now);
    void send(Parent& parent, FixedE9 want, std::span<const VenueBook> books, UnixNanos now);
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
