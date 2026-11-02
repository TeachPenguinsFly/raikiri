#pragma once

// rebuilds what was live from the journal, through the same path the live events take

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/domain/origin.hpp>
#include <raikiri/journal/journal.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/oms/cross_record.hpp>
#include <raikiri/oms/exec_path.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace raikiri::oms {

class OrderReplay {
public:
    OrderReplay(OrderStore& store, ExecPath& path) noexcept : store_{&store}, path_{&path} {}

    void on_cmd(const OrderCmd& cmd);
    void on_event(OrderEvent event);
    // both legs at once; the legs' own event frames that follow it find their executions already booked
    void on_cross(const CrossRecord& record);

    // the highest order seq this strategy has used
    [[nodiscard]] std::uint64_t last_seq(std::uint16_t strategy_id) const noexcept;

    [[nodiscard]] const std::map<std::uint16_t, std::uint64_t>& last_seqs() const noexcept { return last_seq_; }

    [[nodiscard]] std::uint64_t commands() const noexcept { return commands_; }

    [[nodiscard]] std::uint64_t events() const noexcept { return events_; }

    // open orders the store had no room for
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

    [[nodiscard]] const std::vector<CrossRecord>& crosses() const noexcept { return crosses_; }

    // the highest number an internal cross gave a hedge leg
    [[nodiscard]] std::uint64_t last_internal_leg() const noexcept { return last_internal_leg_; }

    // legs a crash left without their event frame, for readers of the journal that rebuild from those frames
    [[nodiscard]] std::vector<OrderEvent> unechoed_legs() const;

private:
    OrderStore* store_;
    ExecPath* path_;
    std::map<std::uint16_t, std::uint64_t> last_seq_;
    std::uint64_t commands_{0};
    std::uint64_t events_{0};
    std::uint64_t dropped_{0};
    std::vector<CrossRecord> crosses_;
    std::vector<ExecId> echoed_;
    std::uint64_t last_internal_leg_{0};
};

// the journal's copy of an order's life; the only encoding of it, so a replay reads what was written
void append_order_cmd(journal::JournalWriter& writer,
                      const OrderCmd& cmd,
                      const OrderOrigin& origin = {},
                      BookKind kind = BookKind::Unknown) noexcept;
void append_order_event(journal::JournalWriter& writer, const OrderEvent& event) noexcept;
void append_cross(journal::JournalWriter& writer, const CrossRecord& record) noexcept;

[[nodiscard]] OrderEvent decode_order_event(const journal::FrameView& frame);
[[nodiscard]] OrderCmd decode_order_cmd(const journal::FrameView& frame);
[[nodiscard]] bool is_cross_frame(const journal::FrameView& frame) noexcept;
[[nodiscard]] CrossRecord decode_cross(const journal::FrameView& frame);

// every frame in order; a missing journal is a first run and visits nothing
void scan_journal(const std::string& dir,
                  const std::string& prefix,
                  const std::function<void(const journal::FrameView&)>& visit);

// the order commands and events; other frames are for their own owners
void replay_orders(const std::string& dir, const std::string& prefix, OrderReplay& replay);

} // namespace raikiri::oms
