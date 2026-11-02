#pragma once

// crosses an arriving client order inside the desk, on the strategy loop, right after the order exists and before any
// of it goes to a venue. all that books goes through the one path a venue fill takes, so state, dedup and replay stay
// one

#include <raikiri/audit/log.hpp>
#include <raikiri/internalcross/application/ports.hpp>
#include <raikiri/internalcross/domain/match.hpp>
#include <raikiri/internalcross/domain/settings.hpp>
#include <raikiri/oms/exec_path.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/order_store.hpp>
#include <raikiri/oms/order_trail.hpp>
#include <raikiri/portfolio/position_book.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::internalcross {

struct Stats {
    std::uint64_t crosses{0};
    std::uint64_t against_clients{0};
    std::uint64_t against_desk{0};
    FixedE9 notional{FixedE9::from_raw(0)};
    FixedE9 saved{FixedE9::from_raw(0)};
    // internal fees clients paid, held by the desk and in no book
    FixedE9 fees{FixedE9::from_raw(0)};
    std::array<std::uint64_t, k_refusal_count> refused{};
    std::map<std::uint16_t, FixedE9> notional_by_book{};
};

struct Arrived {
    QtyLots crossed{QtyLots::from_raw(0)};
    // the rest waits here for an opposite order, and take_due hands it back
    bool held{false};
    std::optional<Refusal> why{};
};

class CrossDesk {
public:
    static constexpr std::size_t k_max_held = 256;
    static constexpr std::size_t k_max_allocations = 32;
    static constexpr std::size_t k_max_touches = 16;

    CrossDesk(CrossKernel& kernel,
              CrossLog& log,
              oms::OrderStore& store,
              oms::ExecPath& exec,
              portfolio::PositionBook& positions,
              oms::OrderTrail& trail,
              audit::Sink& audit);

    void configure(const Settings& settings);

    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

    // a book holds positions only a cross could have given it once this has been on
    [[nodiscard]] bool has_crossed_or_may() const noexcept { return settings_.enabled || stats_.crosses != 0; }

    [[nodiscard]] bool wants(std::uint16_t book, const Instrument& listed);

    // the order is live and submitted; what stays unfilled and not held goes on to the venue
    [[nodiscard]] Arrived on_arrival(ClientOrderId id, const Instrument& listed, UnixNanos now);

    [[nodiscard]] bool holds(ClientOrderId id) const noexcept;
    void forget(ClientOrderId id) noexcept;

    // orders whose wait is over, oldest first; valid until the next call
    [[nodiscard]] std::span<const ClientOrderId> take_due(UnixNanos now) noexcept;

    // a record from the journal, for the running totals
    void restore(const oms::CrossRecord& record);
    void legs_start_after(std::uint64_t seq) noexcept;

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] CrossKernel& kernel() noexcept { return *kernel_; }

private:
    struct Held {
        Interest interest{};
        InstrumentId instrument{};
        std::uint64_t until_ns{0};
    };

    struct BookEntry {
        std::string name{};
        const BookPolicy* policy{nullptr};
    };

    [[nodiscard]] const BookPolicy* policy_of(std::uint16_t book);
    [[nodiscard]] bool room_for(std::uint16_t a, std::uint16_t b, InstrumentId instrument) const noexcept;
    [[nodiscard]] std::optional<ClientOrderId>
    open_desk_leg(std::uint16_t book, const OrderCmd& against, QtyLots qty, PriceTicks price, UnixNanos now);
    [[nodiscard]] bool book_cross(ClientOrderId arriving,
                                  ClientOrderId counter,
                                  bool counter_is_desk,
                                  const Allocation& allocation,
                                  std::int64_t arriving_fee_bp,
                                  const Reference& ref,
                                  const Instrument& listed,
                                  UnixNanos now);
    void audit_cross(const oms::CrossRecord& record, std::string_view symbol, UnixNanos now) noexcept;
    void count(std::optional<Refusal> why) noexcept;
    void add(const oms::CrossRecord& record);
    void settle_held(ClientOrderId id, QtyLots qty) noexcept;

    CrossKernel* kernel_;
    CrossLog* log_;
    oms::OrderStore* store_;
    oms::ExecPath* exec_;
    portfolio::PositionBook* positions_;
    oms::OrderTrail* trail_;
    audit::Sink* audit_;
    Settings settings_{};
    QuotePolicy quotes_{};
    std::unordered_map<std::uint16_t, BookEntry> books_;
    std::vector<Held> held_;
    std::vector<Interest> opposite_;
    std::array<Allocation, k_max_allocations> planned_{};
    std::array<ClientOrderId, 64> due_{};
    oms::IdGen leg_ids_;
    std::uint64_t leg_seq_{0};
    Stats stats_{};
};

} // namespace raikiri::internalcross
