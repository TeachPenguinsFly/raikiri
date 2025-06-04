#include <raikiri/venues/hyperliquid/sign.hpp>

#include <cstring>

namespace raikiri::venues::hyperliquid {

namespace {

// keccak-f[1600]. The permutation is public and fixed; the only thing an
// implementation can get wrong is a table, and a wrong table still produces a
// well-formed 32-byte hash — hence the published vectors in the tests.
constexpr std::array<std::uint64_t, 24> k_round_constants = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

// Rho offsets and the pi lane permutation, walked together as one chain.
constexpr std::array<unsigned, 24> k_rho = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                                            27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
constexpr std::array<std::size_t, 24> k_pi = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                                              15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

// Keccak-256 absorbs 1088 bits at a time; the remaining 512 are the capacity.
constexpr std::size_t k_rate = 136;

[[nodiscard]] constexpr std::uint64_t rotl(std::uint64_t v, unsigned n) noexcept {
    return (v << n) | (v >> (64U - n));
}

void permute(std::array<std::uint64_t, 25>& a) noexcept {
    for (const auto rc : k_round_constants) {
        std::array<std::uint64_t, 5> c{};
        for (std::size_t x = 0; x < 5; ++x) {
            c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        }
        for (std::size_t x = 0; x < 5; ++x) {
            const auto d = c[(x + 4) % 5] ^ rotl(c[(x + 1) % 5], 1);
            for (std::size_t y = 0; y < 25; y += 5) {
                a[y + x] ^= d;
            }
        }

        std::uint64_t carried = a[1];
        for (std::size_t i = 0; i < 24; ++i) {
            const auto lane = k_pi[i];
            const auto held = a[lane];
            a[lane] = rotl(carried, k_rho[i]);
            carried = held;
        }

        for (std::size_t y = 0; y < 25; y += 5) {
            const std::array<std::uint64_t, 5> row = {a[y], a[y + 1], a[y + 2], a[y + 3], a[y + 4]};
            for (std::size_t x = 0; x < 5; ++x) {
                a[y + x] = row[x] ^ (~row[(x + 1) % 5] & row[(x + 2) % 5]);
            }
        }

        a[0] ^= rc;
    }
}

void absorb_block(std::array<std::uint64_t, 25>& state, std::span<const std::uint8_t> block) noexcept {
    for (std::size_t i = 0; i < k_rate; ++i) {
        // Lanes are LITTLE-endian, on every architecture: this is a property of
        // the specification, not of the host.
        state[i / 8] ^= static_cast<std::uint64_t>(block[i]) << (8U * (i % 8));
    }
    permute(state);
}

} // namespace

Hash keccak256(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint64_t, 25> state{};

    std::size_t at = 0;
    for (; at + k_rate <= data.size(); at += k_rate) {
        absorb_block(state, data.subspan(at, k_rate));
    }

    // The padding byte is 0x01, and it is the ONLY difference from NIST
    // SHA3-256, which pads 0x06. Everything else about the two is identical,
    // which is why substituting one for the other fails silently.
    std::array<std::uint8_t, k_rate> tail{};
    const auto remaining = data.size() - at;
    std::memcpy(tail.data(), data.data() + at, remaining);
    tail[remaining] = 0x01;
    tail[k_rate - 1] |= 0x80;
    absorb_block(state, tail);

    Hash out{};
    for (std::size_t i = 0; i < k_hash_bytes; ++i) {
        out[i] = static_cast<std::uint8_t>(state[i / 8] >> (8U * (i % 8)));
    }
    return out;
}

} // namespace raikiri::venues::hyperliquid
