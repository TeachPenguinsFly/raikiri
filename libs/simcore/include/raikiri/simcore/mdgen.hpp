#pragma once

// Deterministic background market-data generator for the sim venue.
//
// Owner-0 ("scenery") limit orders keep ~depth levels quoted around a
// per-instrument integer random-walk mid; occasional owner-0 market orders
// cross the spread so trades print. All math is integer ticks/lots.
//
// Determinism contract: same seed + same call sequence => byte-identical
// book states and event/trade streams. No wall clock, no std::random_device
// — time arrives as UnixNanos parameters and all randomness comes from a
// caller-seeded xoshiro256++ (the UlidGen generator).

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/simcore/engine.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace raikiri::simcore {

class MdGen {
public:
    struct Config {
        std::uint32_t depth{5};               // target quoted levels per side
        std::int64_t level_qty_base{10};      // lots at each level ...
        std::int64_t level_qty_jitter{16};    // ... plus rand % jitter
        std::uint32_t churn_one_in{16};       // odds a live quote is pulled per step
        std::uint32_t market_order_one_in{8}; // odds a step fires a market order
        std::int64_t market_qty_max{12};      // market order size: 1..max lots
    };

    explicit MdGen(std::uint64_t seed) noexcept;
    MdGen(std::uint64_t seed, const Config& cfg) noexcept;

    // Registers an instrument with its starting mid (in ticks). Stepping an
    // unregistered instrument is a no-op: there is no sane default price.
    void add_instrument(InstrumentId instrument, PriceTicks initial_mid);

    // One generator tick: walk the mid, prune/refresh quotes, maybe print a
    // trade. Returns everything the engine emitted — client maker fills ride
    // along when background flow hits resting client orders.
    [[nodiscard]] SimResult step(Engine& engine, InstrumentId instrument, UnixNanos now);

private:
    struct BgOrder {
        std::uint64_t id{0};
        Side side{Side::Buy};
        PriceTicks price{};
    };

    struct InstState {
        PriceTicks mid{};
        std::vector<BgOrder> orders; // quotes we believe are live
    };

    [[nodiscard]] std::uint64_t next_random() noexcept;
    [[nodiscard]] std::uint64_t rand_below(std::uint64_t bound) noexcept;
    void quote_level(Engine& engine, InstrumentId instrument, InstState& st, Side side,
                     PriceTicks price, UnixNanos now, SimResult& out);

    std::uint64_t state_[4]{}; // xoshiro256++ state
    Config cfg_{};
    std::unordered_map<InstrumentId, InstState> instruments_;
};

} // namespace raikiri::simcore
