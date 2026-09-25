#include <raikiri/book/ladder.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>

namespace {

using raikiri::PriceTicks;
using raikiri::QtyLots;
using raikiri::book::Ladder;
using raikiri::book::Level;
using raikiri::book::Side;

Ladder bid_ladder(std::int64_t depth) {
    Ladder bids{Side::Bid};
    for (std::int64_t i = 0; i < depth; ++i) {
        bids.upsert(Level{.price = PriceTicks::from_raw(100'000 - i), .qty = QtyLots::from_raw(1)});
    }
    return bids;
}

void bm_drop_beyond_inside_band(benchmark::State& state) {
    const auto depth = state.range(0);
    auto bids = bid_ladder(depth);
    const auto band = PriceTicks::from_raw(100'000 - depth + 1);
    for (auto _ : state) {
        bids.drop_beyond(band);
        benchmark::DoNotOptimize(bids.size());
    }
}

BENCHMARK(bm_drop_beyond_inside_band)->Arg(50)->Arg(500)->Arg(1000);

void bm_delta_then_prune(benchmark::State& state) {
    const auto depth = state.range(0);
    auto bids = bid_ladder(depth);
    const auto band = PriceTicks::from_raw(100'000 - depth + 1);
    std::int64_t i = 0;
    for (auto _ : state) {
        const auto price = PriceTicks::from_raw(100'000 - (i++ % depth));
        bids.upsert(Level{.price = price, .qty = QtyLots::from_raw(i)});
        bids.upsert(Level{.price = PriceTicks::from_raw(100'000 - depth), .qty = QtyLots::from_raw(1)});
        bids.drop_beyond(band);
        benchmark::DoNotOptimize(bids.size());
    }
}

BENCHMARK(bm_delta_then_prune)->Arg(50)->Arg(500)->Arg(1000);

} // namespace

BENCHMARK_MAIN();
