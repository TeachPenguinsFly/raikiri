#pragma once

// the last prints of the instruments somebody watches; nothing is kept for any other

#include <raikiri/common/time.hpp>
#include <raikiri/domain/instrument.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace raikiri::admin {

inline constexpr std::size_t k_trade_rows = 100;

struct TradePrint {
    PriceTicks price{};
    QtyLots qty{};
    Side aggressor{Side::Buy};
    UnixNanos ts_event{};
};

// a subscriber's cursor is the count of prints it has been sent
class TradeRing {
public:
    void push(const TradePrint& print) noexcept;

    [[nodiscard]] std::uint64_t head() const noexcept { return head_; }

    // the prints after `cursor`, oldest first; a reader further behind than the ring gets what is held
    [[nodiscard]] std::vector<TradePrint> since(std::uint64_t cursor) const;

private:
    std::array<TradePrint, k_trade_rows> prints_{};
    std::uint64_t head_{0};
};

class TradeTape {
public:
    // the ring lives while a subscriber holds it
    [[nodiscard]] std::shared_ptr<TradeRing> watch(InstrumentId id);

    [[nodiscard]] bool watched(InstrumentId id) const noexcept { return rings_.contains(id.packed); }

    // false when nobody watches the instrument
    bool push(InstrumentId id, const TradePrint& print) noexcept;

    // lets go of the rings no subscriber holds; returns how many
    std::size_t prune();

    [[nodiscard]] std::vector<InstrumentId> instruments() const;

private:
    std::unordered_map<std::uint64_t, std::shared_ptr<TradeRing>> rings_;
};

} // namespace raikiri::admin
