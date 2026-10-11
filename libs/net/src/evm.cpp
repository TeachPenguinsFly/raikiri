#include <raikiri/net/evm.hpp>
#include <raikiri/net/signer.hpp>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

#include <cstring>
#include <memory>
#include <vector>

namespace raikiri::net::evm {

namespace {

// wrong round constants still yield a well-formed hash; the tests pin published vectors
constexpr std::array<std::uint64_t, 24> k_round_constants = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL, 0x000000000000808bULL,
    0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL, 0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

// rho offsets and the pi lane permutation, walked as one chain
constexpr std::array<unsigned, 24> k_rho = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                                            27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
constexpr std::array<std::size_t, 24> k_pi = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                                              15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

// rate is 1088 bits; the remaining 512 are capacity
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
        // lanes are little-endian per the spec, independent of the host
        state[i / 8] ^= static_cast<std::uint64_t>(block[i]) << (8U * (i % 8));
    }
    permute(state);
}

constexpr std::size_t k_key_bytes = 32;
constexpr std::string_view k_hex_digits = "0123456789abcdef";

struct BnDelete {
    void operator()(BIGNUM* p) const noexcept { BN_clear_free(p); }
};

struct CtxDelete {
    void operator()(BN_CTX* p) const noexcept { BN_CTX_free(p); }
};

struct GroupDelete {
    void operator()(EC_GROUP* p) const noexcept { EC_GROUP_free(p); }
};

struct PointDelete {
    void operator()(EC_POINT* p) const noexcept { EC_POINT_free(p); }
};

using BnPtr = std::unique_ptr<BIGNUM, BnDelete>;
using CtxPtr = std::unique_ptr<BN_CTX, CtxDelete>;
using GroupPtr = std::unique_ptr<EC_GROUP, GroupDelete>;
using PointPtr = std::unique_ptr<EC_POINT, PointDelete>;

// the signing nonce k is as sensitive as the key, so both are wiped
void wipe(std::span<std::uint8_t> bytes) noexcept {
    auto* volatile at = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        at[i] = 0;
    }
}

[[nodiscard]] int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] bool parse_hex(std::string_view text, std::span<std::uint8_t> out) noexcept {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.size() != out.size() * 2) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto hi = hex_value(text[i * 2]);
        const auto lo = hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

// leading zeroes stripped, as the reference sdk emits
[[nodiscard]] FixedString<66> minimal_hex(std::span<const std::uint8_t> bytes) noexcept {
    std::array<char, 66> buf{};
    buf[0] = '0';
    buf[1] = 'x';
    std::size_t at = 2;
    bool leading = true;
    for (const auto byte : bytes) {
        for (const auto nibble : {static_cast<std::size_t>(byte >> 4), static_cast<std::size_t>(byte & 0x0FU)}) {
            if (leading && nibble == 0) {
                continue;
            }
            leading = false;
            buf[at++] = k_hex_digits[nibble];
        }
    }
    if (at == 2) {
        buf[at++] = '0'; // a zero component is "0x0", never the bare prefix
    }
    const auto text = FixedString<66>::from(std::string_view{buf.data(), at});
    return text.has_value() ? *text : FixedString<66>{};
}

constexpr std::size_t k_address_bytes = 20;

// rfc 6979 deterministic nonce, which makes the published vectors a real test
class Rfc6979 {
public:
    Rfc6979(std::span<const std::uint8_t> key, std::span<const std::uint8_t> msg) noexcept {
        v_.fill(0x01);
        k_.fill(0x00);
        std::array<std::uint8_t, 97> seed{};
        std::memcpy(seed.data() + 33, key.data(), k_key_bytes);
        std::memcpy(seed.data() + 65, msg.data(), k_hash_bytes);
        for (const std::uint8_t tag : {std::uint8_t{0x00}, std::uint8_t{0x01}}) {
            std::memcpy(seed.data(), v_.data(), v_.size());
            seed[32] = tag;
            rekey(as_view(seed));
        }
        wipe(seed);
    }

    ~Rfc6979() {
        wipe(v_);
        wipe(k_);
    }

    Rfc6979(const Rfc6979&) = delete;
    Rfc6979& operator=(const Rfc6979&) = delete;

    [[nodiscard]] std::array<std::uint8_t, 32> next() noexcept {
        if (retried_) {
            std::array<std::uint8_t, 33> seed{};
            std::memcpy(seed.data(), v_.data(), v_.size());
            seed[32] = 0x00;
            rekey(as_view(seed));
            wipe(seed);
        }
        retried_ = true;
        step();
        return v_;
    }

private:
    // written through scratch so nothing relies on the hmac tolerating aliasing
    void rekey(std::string_view extra) noexcept {
        std::array<std::uint8_t, 32> next_key{};
        (void)net::hmac_sha256(key_view(), extra, next_key);
        k_ = next_key;
        wipe(next_key);
        step();
    }

    void step() noexcept {
        std::array<std::uint8_t, 32> next_v{};
        (void)net::hmac_sha256(key_view(), as_view(v_), next_v);
        v_ = next_v;
    }

    template <std::size_t N>
    [[nodiscard]] static std::string_view as_view(const std::array<std::uint8_t, N>& a) noexcept {
        return {reinterpret_cast<const char*>(a.data()), N}; // NOLINT
    }

    [[nodiscard]] std::string_view key_view() const noexcept { return as_view(k_); }

    std::array<std::uint8_t, 32> v_{};
    std::array<std::uint8_t, 32> k_{};
    bool retried_{false};
};

[[nodiscard]] bool to_fixed_bytes(const BIGNUM* value, std::span<std::uint8_t, 32> out) noexcept {
    return BN_bn2binpad(value, out.data(), static_cast<int>(out.size())) == static_cast<int>(out.size());
}

} // namespace

Hash keccak256(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint64_t, 25> state{};

    std::size_t at = 0;
    for (; at + k_rate <= data.size(); at += k_rate) {
        absorb_block(state, data.subspan(at, k_rate));
    }

    // padding byte 0x01; nist sha3-256 pads 0x06
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

Hash keccak256(std::string_view text) noexcept {
    return keccak256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}); // NOLINT
}

bool from_hex(std::string_view text, std::span<std::uint8_t> out) noexcept {
    return parse_hex(text, out);
}

Hash typed_digest(const Hash& domain_separator, const Hash& struct_hash) noexcept {
    std::array<std::uint8_t, 2 + 2 * k_hash_bytes> message{};
    message[0] = 0x19;
    message[1] = 0x01;
    std::memcpy(message.data() + 2, domain_separator.data(), k_hash_bytes);
    std::memcpy(message.data() + 2 + k_hash_bytes, struct_hash.data(), k_hash_bytes);
    return keccak256(message);
}

std::string_view rsv_hex(const Signature& sig, std::span<char> out) noexcept {
    if (out.size() < k_rsv_hex_bytes) {
        return {};
    }
    out[0] = '0';
    out[1] = 'x';
    std::size_t at = 2;
    for (const auto part : {sig.r.view(), sig.s.view()}) {
        const auto digits = part.starts_with("0x") ? part.substr(2) : part;
        if (digits.size() > 64) {
            return {};
        }
        for (std::size_t pad = digits.size(); pad < 64; ++pad) {
            out[at++] = '0';
        }
        digits.copy(out.data() + at, digits.size());
        at += digits.size();
    }
    out[at++] = k_hex_digits[sig.v >> 4U];
    out[at++] = k_hex_digits[sig.v & 0x0FU];
    return {out.data(), at};
}

bool Wallet::load(std::string_view hex_key) noexcept {
    loaded_ = false;
    std::array<std::uint8_t, k_key_bytes> raw{};
    if (!parse_hex(hex_key, raw)) {
        return false;
    }

    const GroupPtr group{EC_GROUP_new_by_curve_name(NID_secp256k1)};
    const CtxPtr ctx{BN_CTX_new()};
    if (group == nullptr || ctx == nullptr) {
        return false;
    }
    const BnPtr secret{BN_bin2bn(raw.data(), static_cast<int>(raw.size()), nullptr)};
    if (secret == nullptr) {
        return false;
    }
    // zero and keys >= n are invalid but still sign
    const BIGNUM* order = EC_GROUP_get0_order(group.get());
    if (BN_is_zero(secret.get()) || BN_cmp(secret.get(), order) >= 0) {
        return false;
    }

    const PointPtr pub{EC_POINT_new(group.get())};
    std::array<std::uint8_t, 65> uncompressed{};
    if (pub == nullptr || EC_POINT_mul(group.get(), pub.get(), secret.get(), nullptr, nullptr, ctx.get()) != 1 ||
        EC_POINT_point2oct(group.get(),
                           pub.get(),
                           POINT_CONVERSION_UNCOMPRESSED,
                           uncompressed.data(),
                           uncompressed.size(),
                           ctx.get()) != uncompressed.size()) {
        return false;
    }
    // last 20 bytes of keccak over the public key without its 0x04 prefix
    const auto hashed = keccak256(std::span{uncompressed}.subspan(1));
    address_[0] = '0';
    address_[1] = 'x';
    for (std::size_t i = 0; i < k_address_bytes; ++i) {
        const auto byte = hashed[k_hash_bytes - k_address_bytes + i];
        address_[2 + i * 2] = k_hex_digits[byte >> 4];
        address_[3 + i * 2] = k_hex_digits[byte & 0x0FU];
    }

    const auto stored = key_.assign(std::string_view{reinterpret_cast<const char*>(raw.data()), raw.size()}); // NOLINT
    wipe(raw);
    if (!stored.has_value()) {
        return false;
    }
    loaded_ = true;
    return true;
}

std::string_view Wallet::address() const noexcept {
    return loaded_ ? std::string_view{address_.data(), address_.size()} : std::string_view{};
}

bool Wallet::sign(const Hash& digest, Signature& out) const noexcept {
    if (!loaded_) {
        return false;
    }
    const GroupPtr group{EC_GROUP_new_by_curve_name(NID_secp256k1)};
    const CtxPtr ctx{BN_CTX_new()};
    if (group == nullptr || ctx == nullptr) {
        return false;
    }
    const BIGNUM* order = EC_GROUP_get0_order(group.get());

    const auto key = key_.view();
    if (key.size() != k_key_bytes) {
        return false;
    }
    const std::span<const std::uint8_t> key_bytes{reinterpret_cast<const std::uint8_t*>(key.data()),
                                                  key.size()}; // NOLINT
    const BnPtr secret{BN_bin2bn(key_bytes.data(), static_cast<int>(key_bytes.size()), nullptr)};

    // rfc 6979 hashes the reduced digest, so reduce first
    const BnPtr z{BN_new()};
    const BnPtr z_raw{BN_bin2bn(digest.data(), static_cast<int>(digest.size()), nullptr)};
    if (secret == nullptr || z == nullptr || z_raw == nullptr ||
        BN_nnmod(z.get(), z_raw.get(), order, ctx.get()) != 1) {
        return false;
    }
    std::array<std::uint8_t, 32> z_bytes{};
    if (!to_fixed_bytes(z.get(), z_bytes)) {
        return false;
    }
    Rfc6979 nonces{key_bytes, z_bytes};
    const BnPtr half{BN_new()};
    const BnPtr r{BN_new()};
    const BnPtr s{BN_new()};
    const BnPtr scratch{BN_new()};
    const BnPtr k_inv{BN_new()};
    const BnPtr x{BN_new()};
    const BnPtr y{BN_new()};
    const PointPtr point{EC_POINT_new(group.get())};
    if (half == nullptr || r == nullptr || s == nullptr || scratch == nullptr || k_inv == nullptr || x == nullptr ||
        y == nullptr || point == nullptr || BN_rshift1(half.get(), order) != 1) {
        return false;
    }

    // retries required by rfc 6979; practically never taken
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto candidate = nonces.next();
        const BnPtr k{BN_bin2bn(candidate.data(), static_cast<int>(candidate.size()), nullptr)};
        wipe(candidate);
        if (k == nullptr || BN_is_zero(k.get()) || BN_cmp(k.get(), order) >= 0) {
            continue;
        }
        if (EC_POINT_mul(group.get(), point.get(), k.get(), nullptr, nullptr, ctx.get()) != 1 ||
            EC_POINT_get_affine_coordinates(group.get(), point.get(), x.get(), y.get(), ctx.get()) != 1 ||
            BN_nnmod(r.get(), x.get(), order, ctx.get()) != 1) {
            return false;
        }
        if (BN_is_zero(r.get())) {
            continue;
        }
        // bit 0 is the parity of R.y, bit 1 marks R.x past the group order
        std::uint8_t recid = BN_is_odd(y.get()) ? 1U : 0U;
        if (BN_cmp(x.get(), order) >= 0) {
            recid |= 2U;
        }
        if (BN_mod_mul(scratch.get(), r.get(), secret.get(), order, ctx.get()) != 1 ||
            BN_mod_add(scratch.get(), scratch.get(), z.get(), order, ctx.get()) != 1 ||
            BN_mod_inverse(k_inv.get(), k.get(), order, ctx.get()) == nullptr ||
            BN_mod_mul(s.get(), scratch.get(), k_inv.get(), order, ctx.get()) != 1) {
            return false;
        }
        if (BN_is_zero(s.get())) {
            continue;
        }
        // low-s normalisation; the venue accepts only the lower s
        if (BN_cmp(s.get(), half.get()) > 0) {
            if (BN_sub(s.get(), order, s.get()) != 1) {
                return false;
            }
            recid ^= 1U;
        }

        std::array<std::uint8_t, 32> r_bytes{};
        std::array<std::uint8_t, 32> s_bytes{};
        if (!to_fixed_bytes(r.get(), r_bytes) || !to_fixed_bytes(s.get(), s_bytes)) {
            return false;
        }
        out.r = minimal_hex(r_bytes);
        out.s = minimal_hex(s_bytes);
        out.v = static_cast<std::uint8_t>(27U + recid);
        return true;
    }
    return false;
}

} // namespace raikiri::net::evm
