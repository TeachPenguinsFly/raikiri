#include <raikiri/simcore/mdgen.hpp>

namespace raikiri::simcore {

namespace {

// splitmix64 / xoshiro256++ — the UlidGen generator, duplicated rather than
// shared because the two streams have unrelated lifetimes and must never
// perturb each other's determinism.
constexpr std::uint64_t splitmix64(std::uint64_t& x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

constexpr std::uint64_t rotl(std::uint64_t v, int k) noexcept {
    return (v << k) | (v >> (64 - k));
}

} // namespace

MdGen::MdGen(std::uint64_t seed) noexcept : MdGen{seed, Config{}} {}

MdGen::MdGen(std::uint64_t seed, const Config& cfg) noexcept : cfg_{cfg} {
    for (auto& s : state_) {
        s = splitmix64(seed);
    }
}

// xoshiro256++
std::uint64_t MdGen::next_random() noexcept {
    const std::uint64_t result = rotl(state_[0] + state_[3], 23) + state_[0];
    const std::uint64_t t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
}

std::uint64_t MdGen::rand_below(std::uint64_t bound) noexcept {
    // Modulo bias is acceptable: this randomness shapes scenery, not
    // statistics — determinism, not uniformity, is the requirement.
    return next_random() % bound;
}

void MdGen::add_instrument(InstrumentId instrument, PriceTicks initial_mid) {
    InstState st;
    st.mid = initial_mid;
    instruments_.insert_or_assign(instrument, std::move(st));
}

SimResult MdGen::step(Engine& engine, InstrumentId instrument, UnixNanos now) {
    SimResult out;
    const auto it = instruments_.find(instrument);
    if (it == instruments_.end()) {
        return out; // unregistered: no default price exists, so do nothing
    }
    InstState& st = it->second;

    // Integer random walk: -1, 0 or +1 tick per step, floored so the deepest
    // bid level always stays at a positive price.
    const std::int64_t drift = static_cast<std::int64_t>(rand_below(3)) - 1;
    const std::int64_t floor_ticks = static_cast<std::int64_t>(cfg_.depth) + 2;
    std::int64_t mid = st.mid.raw + drift;
    if (mid < floor_ticks) {
        mid = floor_ticks;
    }
    st.mid = PriceTicks::from_raw(mid);

    // Prune pass: forget quotes a taker already consumed, pull quotes that
    // drifted out of the band, and randomly refresh some for churn.
    const Book* book = engine.book(instrument);
    std::vector<BgOrder> keep;
    keep.reserve(st.orders.size());
    for (const BgOrder& order : st.orders) {
        if (book == nullptr || !book->contains(order.id)) {
            continue; // consumed or never rested — nothing to cancel
        }
        const std::int64_t dist =
            order.side == Side::Buy ? mid - order.price.raw : order.price.raw - mid;
        const bool out_of_band = dist < 1 || dist > static_cast<std::int64_t>(cfg_.depth);
        const bool churned = cfg_.churn_one_in != 0 && rand_below(cfg_.churn_one_in) == 0;
        if (out_of_band || churned) {
            engine.cancel_background(instrument, order.id, &out);
            continue;
        }
        keep.push_back(order);
    }
    st.orders = std::move(keep);

    // Refill pass: one quote per level on both sides. A fresh quote may cross
    // a client order resting inside the band — those fills (maker events +
    // trades) accumulate into `out` via the engine.
    for (std::uint32_t lvl = 1; lvl <= cfg_.depth; ++lvl) {
        const std::int64_t d = static_cast<std::int64_t>(lvl);
        quote_level(engine, instrument, st, Side::Buy, PriceTicks::from_raw(mid - d), now, out);
        quote_level(engine, instrument, st, Side::Sell, PriceTicks::from_raw(mid + d), now, out);
    }

    // Occasionally cross the spread with a market order so trades print.
    if (cfg_.market_order_one_in != 0 && rand_below(cfg_.market_order_one_in) == 0) {
        const Side side = (next_random() & 1U) == 0 ? Side::Buy : Side::Sell;
        const std::uint64_t span =
            cfg_.market_qty_max > 0 ? static_cast<std::uint64_t>(cfg_.market_qty_max) : 1U;
        const std::int64_t qty = 1 + static_cast<std::int64_t>(rand_below(span));
        engine.place_background_market(instrument, side, QtyLots::from_raw(qty), now, out);
    }

    return out;
}

void MdGen::quote_level(Engine& engine, InstrumentId instrument, InstState& st, Side side,
                        PriceTicks price, UnixNanos now, SimResult& out) {
    for (const BgOrder& order : st.orders) {
        if (order.side == side && order.price == price) {
            return; // level already quoted
        }
    }
    const std::int64_t jitter =
        cfg_.level_qty_jitter > 0
            ? static_cast<std::int64_t>(rand_below(static_cast<std::uint64_t>(cfg_.level_qty_jitter)))
            : 0;
    const QtyLots qty = QtyLots::from_raw(cfg_.level_qty_base + jitter);
    const std::uint64_t id =
        engine.place_background_limit(instrument, side, price, qty, now, out);
    // Track the id even if the quote crossed and fully filled on placement —
    // the next prune pass drops ids the book no longer contains.
    st.orders.push_back(BgOrder{id, side, price});
}

} // namespace raikiri::simcore
