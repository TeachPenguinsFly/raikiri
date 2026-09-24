#pragma once

// deterministic: the same seed and call sequence give byte-identical books and streams

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

    void add_instrument(InstrumentId instrument, PriceTicks initial_mid);

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
    void quote_level(Engine& engine,
                     InstrumentId instrument,
                     InstState& st,
                     Side side,
                     PriceTicks price,
                     UnixNanos now,
                     SimResult& out);

    std::uint64_t state_[4]{}; // xoshiro256++ state
    Config cfg_{};
    std::unordered_map<InstrumentId, InstState> instruments_;
};

} // namespace raikiri::simcore
