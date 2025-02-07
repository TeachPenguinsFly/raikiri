#include <raikiri/common/ulid.hpp>

#include <raikiri/common/int128.hpp>

#include <random>

namespace raikiri {

namespace {

// Crockford base32: no I, L, O, U.
constexpr char k_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// splitmix64 — seeds the xoshiro state from one 64-bit value.
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

UlidGen::UlidGen() {
    std::random_device rd;
    std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    for (auto& s : state_) {
        s = splitmix64(seed);
    }
}

UlidGen::UlidGen(std::uint64_t seed) noexcept {
    for (auto& s : state_) {
        s = splitmix64(seed);
    }
}

// xoshiro256++
std::uint64_t UlidGen::next_random() noexcept {
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

Ulid UlidGen::next(UnixNanos wall) noexcept {
    const std::uint64_t ms = wall.ns / 1'000'000U;
    if (ms == last_ms_) {
        // Same millisecond: increment the 80-bit random part so ids stay
        // strictly ordered and unique without re-rolling.
        if (++rand_lo_ == 0) {
            rand_hi_ = (rand_hi_ + 1) & 0xFFFFU;
        }
    } else {
        last_ms_ = ms;
        rand_hi_ = next_random() & 0xFFFFU;
        rand_lo_ = next_random();
    }

    // Layout: 48 bits time | 16 bits rand_hi | 64 bits rand_lo = 128 bits,
    // encoded as 26 base32 chars (10 for time, 16 for randomness).
    Ulid out{};
    out.len = 26;

    std::uint64_t t = ms & 0xFFFF'FFFF'FFFFULL;
    for (int i = 9; i >= 0; --i) {
        out.data[static_cast<std::size_t>(i)] = k_alphabet[t & 0x1FU];
        t >>= 5;
    }

    // 80 random bits -> 16 chars, most significant first.
    uint128_t r = (static_cast<uint128_t>(rand_hi_) << 64) | rand_lo_;
    for (int i = 25; i >= 10; --i) {
        out.data[static_cast<std::size_t>(i)] = k_alphabet[static_cast<std::uint8_t>(r & 0x1FU)];
        r >>= 5;
    }
    return out;
}

} // namespace raikiri
