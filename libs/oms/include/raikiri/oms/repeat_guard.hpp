#pragma once

// refuses a command that repeats an order just sent, whichever path it came by; allocates nothing after construction

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace raikiri::oms {

// what makes two orders the same. `repeat_key` is the caller's name for a deliberate repeat: a slicing algo's parent
// and child ordinal, so its identical slices differ while a true resend of one slice does not
struct OrderShape {
    std::uint16_t book{0};
    InstrumentId instrument{};
    Side side{Side::Buy};
    PriceTicks price{};
    QtyLots qty{};
    std::uint64_t repeat_key{0};

    friend constexpr bool operator==(const OrderShape&, const OrderShape&) = default;
};

class RepeatGuard {
public:
    static constexpr std::size_t k_default_slots = 4096;

    explicit RepeatGuard(std::size_t slots = k_default_slots);

    // the earlier order's id, when one of this shape went out inside the window and has not been withdrawn; a zero
    // window is a book that repeats on purpose
    [[nodiscard]] std::optional<ClientOrderId>
    held(const OrderShape& shape, UnixNanos now, std::uint64_t window_ns) const noexcept;

    void sent(const OrderShape& shape, ClientOrderId id, UnixNanos now) noexcept;

    // the order can no longer execute, or was asked to stop: a new one is a new decision. an order that filled is
    // never withdrawn, since a double click after an instant fill is the case this guard exists for
    void withdrawn(ClientOrderId id) noexcept;

private:
    struct Sent {
        OrderShape shape{};
        ClientOrderId id{};
        std::uint64_t at_ns{0};
        bool withdrawn{false};
    };

    std::vector<Sent> sent_;
    std::size_t next_{0};
};

} // namespace raikiri::oms
