// Single-process microbenchmarks for the shm primitives (Google Benchmark).
// Cross-process latency lives in shm_pingpong; these track per-op costs and
// regressions in the primitives themselves.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/shm/broadcast_ring.hpp>
#include <raikiri/shm/seqlock_table.hpp>
#include <raikiri/shm/spsc_ring.hpp>

#include <benchmark/benchmark.h>

#include <memory>

namespace {

using raikiri::Bbo;
using raikiri::InstrumentId;
using raikiri::MarketType;
using raikiri::PriceTicks;
using raikiri::QtyLots;
using raikiri::SymbolId;
using raikiri::VenueId;

Bbo sample_bbo(std::uint64_t i) {
    Bbo bbo{};
    bbo.instrument = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1});
    bbo.bid = PriceTicks::from_raw(static_cast<std::int64_t>(500'000 + (i % 100)));
    bbo.ask = PriceTicks::from_raw(static_cast<std::int64_t>(500'010 + (i % 100)));
    bbo.bid_qty = QtyLots::from_raw(25);
    bbo.ask_qty = QtyLots::from_raw(40);
    bbo.ts_event = {i};
    bbo.ts_init = {i};
    return bbo;
}

void bm_seqlock_write(benchmark::State& state) {
    auto cell = std::make_unique<raikiri::shm::SeqlockCell<Bbo>>();
    std::uint64_t i = 0;
    for (auto _ : state) {
        cell->write(sample_bbo(++i));
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(i));
}

void bm_seqlock_read(benchmark::State& state) {
    auto cell = std::make_unique<raikiri::shm::SeqlockCell<Bbo>>();
    cell->write(sample_bbo(1));
    Bbo out{};
    std::uint64_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(cell->read(out));
        ++i;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(i));
}

void bm_spsc_push_pop(benchmark::State& state) {
    auto ring = std::make_unique<raikiri::shm::SpscRing<std::uint64_t, 4096>>();
    raikiri::shm::SpscProducer producer{ring.get()};
    raikiri::shm::SpscConsumer consumer{ring.get()};
    std::uint64_t i = 0;
    std::uint64_t out{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(producer.try_push(++i).has_value());
        benchmark::DoNotOptimize(consumer.try_pop(out));
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(i));
}

void bm_broadcast_publish(benchmark::State& state) {
    auto ring = std::make_unique<raikiri::shm::BroadcastRing<Bbo, 65536>>();
    std::uint64_t i = 0;
    for (auto _ : state) {
        ring->publish(sample_bbo(++i));
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(i));
}

BENCHMARK(bm_seqlock_write);
BENCHMARK(bm_seqlock_read);
BENCHMARK(bm_spsc_push_pop);
BENCHMARK(bm_broadcast_publish);

} // namespace

BENCHMARK_MAIN();
