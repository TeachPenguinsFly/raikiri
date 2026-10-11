#include <raikiri/venues/lighter/sign.hpp>

#include <openssl/crypto.h>
#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace raikiri::venues::lighter {

namespace {

using u64 = std::uint64_t;
using u128 = unsigned __int128;

constexpr u64 k_p = 0xFFFFFFFF00000001ULL;
constexpr u64 k_epsilon = 0xFFFFFFFFULL;

[[nodiscard]] constexpr u64 canonical(u64 x) noexcept {
    return x >= k_p ? x - k_p : x;
}

[[nodiscard]] constexpr u64 reduce(u128 x) noexcept {
    const auto lo = static_cast<u64>(x);
    const auto hi = static_cast<u64>(x >> 64);
    u64 t0 = lo - (hi >> 32);
    if (lo < (hi >> 32)) {
        t0 -= k_epsilon;
    }
    const u64 t1 = (hi & k_epsilon) * k_epsilon;
    u64 sum = t0 + t1;
    if (sum < t1) {
        sum += k_epsilon;
    }
    return canonical(sum);
}

[[nodiscard]] constexpr u64 fadd(u64 a, u64 b) noexcept {
    return reduce(static_cast<u128>(a) + b);
}

[[nodiscard]] constexpr u64 fsub(u64 a, u64 b) noexcept {
    return a >= b ? a - b : a + (k_p - b);
}

[[nodiscard]] constexpr u64 fmul(u64 a, u64 b) noexcept {
    return reduce(static_cast<u128>(a) * b);
}

[[nodiscard]] constexpr u64 fpow(u64 base, u64 exponent) noexcept {
    u64 result = 1;
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            result = fmul(result, base);
        }
        base = fmul(base, base);
        exponent >>= 1U;
    }
    return result;
}

// poseidon2, plonky2 parameters

constexpr std::size_t k_width = 12;
constexpr std::size_t k_rate = 8;
constexpr std::size_t k_rounds_f = 8;
constexpr std::size_t k_rounds_p = 22;

constexpr std::array<std::array<std::uint64_t, k_width>, k_rounds_f> k_external{{
    {{0xD70193D17AB3B7D6,
      0xA2C3662A78A9162B,
      0x7A9FDA827556AD44,
      0xE8D5501818C99643,
      0x4C7A8FCED4D5FD38,
      0x55AB38985C0C513D,
      0x28A17BD016210B0B,
      0x8F8277679EC32FA8,
      0x768B3C3D68A460E9,
      0x872A022EB559D941,
      0xD1316DD4B3B97973,
      0xA7B608E578321000}},
    {{0x3FA02C87B0BEE026,
      0x7A38F0022E13C31E,
      0x00C054F3C5E8D20D,
      0x439F50F4BCA7242F,
      0x4D0938AA57CD517F,
      0xB2E03AC5FB6B9A7D,
      0xE29D1F4237BEDCA8,
      0x05B7C844BC99B848,
      0x91CC0B73F34E17ED,
      0x876E4427694BD755,
      0x67002AE0725C612D,
      0x05351F20E0B6315F}},
    {{0x2E3B9EF5457EB60B,
      0xD9AC17618C3783DD,
      0x0807528AD8874BCF,
      0xC78D546A455D2A0E,
      0xF8B930C81E2481F0,
      0x712707D8DFF3B041,
      0xDCB8C0AA0B9D34C3,
      0x9BADDBDF2EE3A468,
      0x2DD16D50C5176C78,
      0x89EAC5CFBC075CD3,
      0x2A741DEA181587F3,
      0x1A4D6AA85A113D84}},
    {{0x4D736286A2387E34,
      0x8BAD5DFC4FCB3EE3,
      0x84FBD03ADB77C56A,
      0x8D5CDD1A23EC53A2,
      0x036F08F08FFF28EC,
      0xB717A3F4DBDFB443,
      0x58A074B5509D645C,
      0xF92BF834E4B87718,
      0x1541C3A0BAA5AC4B,
      0x22149E6783E67692,
      0x9BE8B5D9E112476F,
      0x41E0969F62BABB76}},
    {{0xBC585AD3B9443DBB,
      0xF28DD3206975CBB1,
      0xDD8815E53CA045E0,
      0xDE82C416B9E701BA,
      0xC5CB875233AFA025,
      0x7212697CD897FFA9,
      0x67844790AA63CFD7,
      0xDC0B9CFA97FE65C3,
      0xE8FE091869A82070,
      0x62902BB2E413C6D1,
      0x29F9F5001FB84F57,
      0xBE1014796EF5F8BE}},
    {{0x71FEB53E9BDBA19C,
      0x251054F592EBB71C,
      0xE1A57643A4BB284B,
      0xA4BA6F87A45B739B,
      0x2C1FCADE0B958C49,
      0xBBB424CDA9A3E360,
      0x2CA647354C5F3F54,
      0xC9277B64D152E084,
      0xDBC9AC97445EFF17,
      0x6F6CDF3198969F70,
      0x1DE29D14FA76D8F1,
      0x73337458A8CC1D19}},
    {{0xB87E775E2FB3AB23,
      0xF166A1C7A565C80B,
      0xB24BE06F426C747F,
      0xC281E8C49482CE00,
      0x51974C3B3B726C2D,
      0x87444CF8CAF7D619,
      0x7C362F827A580CED,
      0x9567AF14667647A0,
      0xCBF0473CBEC54E37,
      0xE3209DEDEFF4F620,
      0xD43AD94E45A4C4EE,
      0x976981EE73F41768}},
    {{0xEF707A224E207258,
      0x2FC779E10E6362EE,
      0x29B5EE60AD8C891F,
      0x96B37B39D8BFD667,
      0x877DF68A8B22E733,
      0x5C41746F562C8D9F,
      0x0C9D76751052B71A,
      0xFB3465341BF1C087,
      0xA0D14DC614D15EB1,
      0xDC27D17136906FA6,
      0x482E163B05EC397F,
      0x0273A462992366EF}},
}};
constexpr std::array<std::uint64_t, k_rounds_p> k_internal{
    0xA571418D95897B60, 0x8F32676574FCF6D3, 0x731102D4E3FB1BBE, 0x0330F08328A82D2B, 0x7F0449B6557F785D,
    0x62F06210658DCBCB, 0xD5A98AF9F89C458B, 0x77EC69083A346385, 0xEF7CA48BBC27F890, 0x53E9652F61EAC532,
    0xA71C634ABFF4F0CC, 0xB16F5F0D7E28EA29, 0xC9DDE31D0A003AB2, 0x2DDADF9775902533, 0xE4FA73FB16408B47,
    0x90242EBC00D2EE59, 0xBB02DFFD9F381982, 0xDEA328364C50907C, 0x1395D3B924857CF8, 0x7D3EAD0D5AEC04E6,
    0xC2F12BE3FED74668, 0x0BA3C338F8C3D285,
};
constexpr std::array<std::uint64_t, k_width> k_diagonal{
    0xC3B6C08E23BA9300,
    0xD84B5DE94A324FB6,
    0x0D0C371C5B35B84F,
    0x7964F570E7188037,
    0x5DAF18BBD996604B,
    0x6743BC47B9595257,
    0x5528B9362C59BB70,
    0xAC45E25B7127B68B,
    0xA2077D7DFBB606B5,
    0xF3FAAC6FAEE378AE,
    0x0C6388B51545E883,
    0xD27DBB6944917B60,
};

using State = std::array<u64, k_width>;

void external_layer(State& s) noexcept {
    std::array<u128, k_width> w{};
    for (std::size_t i = 0; i < k_width; i += 4) {
        const u128 x0 = s[i];
        const u128 x1 = s[i + 1];
        const u128 x2 = s[i + 2];
        const u128 x3 = s[i + 3];
        const u128 t01 = x0 + x1;
        const u128 t23 = x2 + x3;
        const u128 t0123 = t01 + t23;
        w[i] = t0123 + t01 + x1;
        w[i + 1] = t0123 + x1 + x2 + x2;
        w[i + 2] = t0123 + t23 + x3;
        w[i + 3] = t0123 + x3 + x0 + x0;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const u128 sum = w[i] + w[i + 4] + w[i + 8];
        for (std::size_t j = i; j < k_width; j += 4) {
            s[j] = reduce(w[j] + sum);
        }
    }
}

void internal_layer(State& s) noexcept {
    u128 total = 0;
    for (const auto limb : s) {
        total += limb;
    }
    const u64 sum = reduce(total);
    for (std::size_t i = 0; i < k_width; ++i) {
        s[i] = reduce(static_cast<u128>(s[i]) * k_diagonal[i] + sum);
    }
}

[[nodiscard]] u64 sbox(u64 x) noexcept {
    const u64 x2 = fmul(x, x);
    const u64 x3 = fmul(x2, x);
    return fmul(fmul(x3, x3), x);
}

void permute(State& s) noexcept {
    external_layer(s);
    for (std::size_t r = 0; r < k_rounds_f; ++r) {
        if (r == k_rounds_f / 2) {
            for (std::size_t p = 0; p < k_rounds_p; ++p) {
                s[0] = sbox(fadd(s[0], k_internal[p]));
                internal_layer(s);
            }
        }
        for (std::size_t i = 0; i < k_width; ++i) {
            s[i] = sbox(fadd(s[i], k_external[r][i]));
        }
        external_layer(s);
    }
}

// the quintic extension, x^5 = 3

using Fp5 = std::array<u64, 5>;

constexpr u64 k_w = 3;
constexpr u64 k_fifth_root = 1041288259238279555ULL;

[[nodiscard]] Fp5 add(const Fp5& a, const Fp5& b) noexcept {
    return {fadd(a[0], b[0]), fadd(a[1], b[1]), fadd(a[2], b[2]), fadd(a[3], b[3]), fadd(a[4], b[4])};
}

[[nodiscard]] Fp5 sub(const Fp5& a, const Fp5& b) noexcept {
    return {fsub(a[0], b[0]), fsub(a[1], b[1]), fsub(a[2], b[2]), fsub(a[3], b[3]), fsub(a[4], b[4])};
}

[[nodiscard]] Fp5 mul(const Fp5& a, const Fp5& b) noexcept {
    std::array<u128, 5> low{};
    std::array<u128, 5> wrap{};
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 5; ++j) {
            const u64 product = fmul(a[i], b[j]);
            if (i + j < 5) {
                low[i + j] += product;
            } else {
                wrap[i + j - 5] += product;
            }
        }
    }
    Fp5 out{};
    for (std::size_t i = 0; i < 5; ++i) {
        out[i] = fadd(reduce(low[i]), fmul(k_w, reduce(wrap[i])));
    }
    return out;
}

[[nodiscard]] Fp5 scale(const Fp5& a, u64 k) noexcept {
    return {fmul(a[0], k), fmul(a[1], k), fmul(a[2], k), fmul(a[3], k), fmul(a[4], k)};
}

[[nodiscard]] bool is_zero(const Fp5& a) noexcept {
    return (a[0] | a[1] | a[2] | a[3] | a[4]) == 0;
}

[[nodiscard]] Fp5 frobenius(const Fp5& a, unsigned count) noexcept {
    const u64 z = fpow(k_fifth_root, count);
    Fp5 out{};
    u64 power = 1;
    for (std::size_t i = 0; i < 5; ++i) {
        out[i] = fmul(a[i], power);
        power = fmul(power, z);
    }
    return out;
}

// a^(r-1) by frobenius, so a * f lies in the base field
[[nodiscard]] Fp5 inverse_or_zero(const Fp5& a) noexcept {
    if (is_zero(a)) {
        return {};
    }
    const Fp5 d = frobenius(a, 1);
    const Fp5 e = mul(d, frobenius(d, 1));
    const Fp5 f = mul(e, frobenius(e, 2));
    const u64 norm = mul(a, f)[0];
    return scale(f, fpow(norm, k_p - 2));
}

// ecgfp5 in (x, z, u, t) coordinates, w = t / u

struct Point {
    Fp5 x{};
    Fp5 z{1, 0, 0, 0, 0};
    Fp5 u{};
    Fp5 t{1, 0, 0, 0, 0};
};

constexpr Fp5 k_b{0, 263, 0, 0, 0};
constexpr Fp5 k_b2{0, 526, 0, 0, 0};
constexpr Fp5 k_b4{0, 1052, 0, 0, 0};
constexpr Fp5 k_four{4, 0, 0, 0, 0};

constexpr Point k_generator{
    .x = {12883135586176881569ULL,
          4356519642755055268ULL,
          5248930565894896907ULL,
          2165973894480315022ULL,
          2448410071095648785ULL},
    .z = {1, 0, 0, 0, 0},
    .u = {1, 0, 0, 0, 0},
    .t = {4, 0, 0, 0, 0},
};

[[nodiscard]] Point point_add(const Point& p, const Point& q) noexcept {
    const Fp5 t1 = mul(p.x, q.x);
    const Fp5 t2 = mul(p.z, q.z);
    const Fp5 t3 = mul(p.u, q.u);
    const Fp5 t4 = mul(p.t, q.t);
    const Fp5 t5 = sub(mul(add(p.x, p.z), add(q.x, q.z)), add(t1, t2));
    const Fp5 t6 = sub(mul(add(p.u, p.t), add(q.u, q.t)), add(t3, t4));
    const Fp5 t7 = add(t1, mul(t2, k_b));
    const Fp5 t8 = mul(t4, t7);
    const Fp5 t9 = mul(t3, add(mul(t5, k_b2), add(t7, t7)));
    const Fp5 t10 = mul(add(t4, add(t3, t3)), add(t5, t7));
    return {.x = mul(sub(t10, t8), k_b), .z = sub(t8, t9), .u = mul(t6, sub(mul(t2, k_b), t1)), .t = add(t8, t9)};
}

[[nodiscard]] Point point_double(const Point& p) noexcept {
    const Fp5 t1 = mul(p.z, p.t);
    const Fp5 t2 = mul(t1, p.t);
    const Fp5 x1 = mul(t2, t2);
    const Fp5 z1 = mul(t1, p.u);
    const Fp5 t3 = mul(p.u, p.u);
    const Fp5 xz = add(p.x, p.z);
    const Fp5 w1 = sub(t2, mul(t3, add(xz, xz)));
    const Fp5 t4 = mul(z1, z1);
    const Fp5 z = mul(w1, w1);
    const Fp5 wz = add(w1, z1);
    return {
        .x = mul(t4, k_b4), .z = z, .u = sub(mul(wz, wz), add(t4, z)), .t = sub(add(x1, x1), add(mul(t4, k_four), z))};
}

[[nodiscard]] Fp5 encode(const Point& p) noexcept {
    return mul(p.t, inverse_or_zero(p.u));
}

// the scalar field, n prime and just under 2^319

using Scalar = std::array<u64, 5>;

constexpr Scalar k_n{
    0xE80FD996948BFFE1ULL, 0xE8885C39D724A09CULL, 0x7FFFFFE6CFB80639ULL, 0x7FFFFFF100000016ULL, 0x7FFFFFFD80000007ULL};
constexpr u64 k_n0_inverse = 0xD78BEF72057B7BDFULL;
constexpr Scalar k_r2{
    0xA01001DCE33DC739ULL, 0x6C3228D33F62ACCFULL, 0xD1D796CC91CF8525ULL, 0xAADFFF5D1574C1D8ULL, 0x4ACA13B28CA251F5ULL};

// a - b over five limbs; true on borrow
[[nodiscard]] bool sub_limbs(const Scalar& a, const Scalar& b, Scalar& out) noexcept {
    u64 borrow = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        const u128 d = static_cast<u128>(a[i]) - b[i] - borrow;
        out[i] = static_cast<u64>(d);
        borrow = static_cast<u64>(d >> 64) & 1U;
    }
    return borrow != 0;
}

void add_limbs(const Scalar& a, const Scalar& b, Scalar& out) noexcept {
    u64 carry = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        const u128 s = static_cast<u128>(a[i]) + b[i] + carry;
        out[i] = static_cast<u64>(s);
        carry = static_cast<u64>(s >> 64);
    }
}

// any five limbs below 2^320, which is under 3n
[[nodiscard]] Scalar reduce_scalar(Scalar a) noexcept {
    Scalar less{};
    while (!sub_limbs(a, k_n, less)) {
        a = less;
    }
    return a;
}

[[nodiscard]] Scalar scalar_sub(const Scalar& a, const Scalar& b) noexcept {
    Scalar out{};
    if (sub_limbs(a, b, out)) {
        Scalar wrapped{};
        add_limbs(out, k_n, wrapped);
        return wrapped;
    }
    return out;
}

// a * b / 2^320 mod n, for a below n
[[nodiscard]] Scalar monty_mul(const Scalar& a, const Scalar& b) noexcept {
    Scalar r{};
    for (std::size_t i = 0; i < 5; ++i) {
        const u64 m = b[i];
        const u64 f = (a[0] * m + r[0]) * k_n0_inverse;
        u64 cc1 = 0;
        u64 cc2 = 0;
        for (std::size_t j = 0; j < 5; ++j) {
            const u128 z = static_cast<u128>(a[j]) * m + r[j] + cc1;
            cc1 = static_cast<u64>(z >> 64);
            const u128 y = static_cast<u128>(f) * k_n[j] + static_cast<u64>(z) + cc2;
            cc2 = static_cast<u64>(y >> 64);
            if (j > 0) {
                r[j - 1] = static_cast<u64>(y);
            }
        }
        r[4] = cc1 + cc2;
    }
    Scalar less{};
    return sub_limbs(r, k_n, less) ? r : less;
}

[[nodiscard]] Scalar scalar_mul(const Scalar& a, const Scalar& b) noexcept {
    return monty_mul(monty_mul(a, k_r2), b);
}

[[nodiscard]] Scalar scalar_from_bytes(std::span<const std::uint8_t> bytes) noexcept {
    Scalar out{};
    for (std::size_t i = 0; i < bytes.size() && i < 40; ++i) {
        out[i / 8] |= static_cast<u64>(bytes[i]) << (8 * (i % 8));
    }
    return out;
}

void scalar_to_bytes(const Scalar& s, std::span<std::uint8_t, 40> out) noexcept {
    for (std::size_t i = 0; i < 40; ++i) {
        out[i] = static_cast<std::uint8_t>(s[i / 8] >> (8 * (i % 8)));
    }
}

// lo + hi * 2^320 mod n, hi taken from up to three more limbs
[[nodiscard]] Scalar reduce_wide(std::span<const std::uint8_t, 64> bytes) noexcept {
    const Scalar lo = reduce_scalar(scalar_from_bytes(bytes.first<40>()));
    const Scalar hi = scalar_from_bytes(bytes.subspan<40>());
    Scalar sum{};
    add_limbs(lo, monty_mul(hi, k_r2), sum);
    return reduce_scalar(sum);
}

[[nodiscard]] Digest to_digest(const Fp5& a) noexcept {
    Digest out{};
    for (std::size_t i = 0; i < 40; ++i) {
        out[i] = static_cast<std::uint8_t>(canonical(a[i / 8]) >> (8 * (i % 8)));
    }
    return out;
}

[[nodiscard]] Fp5 from_digest(const Digest& d) noexcept {
    const auto limbs = scalar_from_bytes(d);
    return {canonical(limbs[0]), canonical(limbs[1]), canonical(limbs[2]), canonical(limbs[3]), canonical(limbs[4])};
}

// 16^i * d * G for every 4-bit digit d
using BaseTable = std::array<std::array<Point, 16>, 80>;

[[nodiscard]] std::unique_ptr<const BaseTable> make_base_table() {
    auto table = std::make_unique<BaseTable>();
    Point base = k_generator;
    for (auto& row : *table) {
        row[0] = Point{};
        row[1] = base;
        for (std::size_t d = 2; d < 16; ++d) {
            row[d] = point_add(row[d - 1], base);
        }
        base = point_double(point_double(point_double(point_double(base))));
    }
    return table;
}

[[nodiscard]] Fp5 base_mul(const Scalar& k) noexcept {
    static const auto table = make_base_table();
    Point acc{};
    for (std::size_t i = 0; i < 80; ++i) {
        const auto digit = static_cast<std::size_t>(k[i / 16] >> (4 * (i % 16))) & 0x0FU;
        acc = point_add(acc, (*table)[i][digit]);
    }
    return encode(acc);
}

[[nodiscard]] Signature schnorr(const Scalar& secret, const Digest& digest, const Scalar& nonce) noexcept {
    const Fp5 r = base_mul(nonce);
    const Fp5 m = from_digest(digest);
    const std::array<u64, 10> preimage{r[0], r[1], r[2], r[3], r[4], m[0], m[1], m[2], m[3], m[4]};
    const auto challenge = hash_to_quintic(preimage);
    const Scalar e = reduce_scalar(scalar_from_bytes(challenge));
    const Scalar s = scalar_sub(nonce, scalar_mul(e, secret));
    Signature out{};
    scalar_to_bytes(s, std::span{out}.first<40>());
    scalar_to_bytes(e, std::span{out}.subspan<40>());
    return out;
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

} // namespace

Digest hash_to_quintic(std::span<const std::uint64_t> elements) noexcept {
    State state{};
    for (std::size_t i = 0; i < elements.size(); i += k_rate) {
        for (std::size_t j = 0; j < k_rate && i + j < elements.size(); ++j) {
            state[j] = canonical(elements[i + j]);
        }
        permute(state);
    }
    return to_digest(Fp5{state[0], state[1], state[2], state[3], state[4]});
}

Digest encode_base_mul(std::span<const std::uint8_t, k_digest_bytes> scalar) noexcept {
    return to_digest(base_mul(reduce_scalar(scalar_from_bytes(scalar))));
}

Signature sign_with_nonce(std::span<const std::uint8_t, k_digest_bytes> secret,
                          const Digest& digest,
                          std::span<const std::uint8_t, k_digest_bytes> nonce) noexcept {
    return schnorr(reduce_scalar(scalar_from_bytes(secret)), digest, reduce_scalar(scalar_from_bytes(nonce)));
}

bool ApiKey::load(std::string_view hex_key) noexcept {
    loaded_ = false;
    key_.clear();
    if (hex_key.starts_with("0x")) {
        hex_key.remove_prefix(2);
    }
    if (hex_key.size() != k_digest_bytes * 2) {
        return false;
    }
    std::array<std::uint8_t, k_digest_bytes> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const int hi = hex_value(hex_key[2 * i]);
        const int lo = hex_value(hex_key[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        raw[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    const Scalar secret = reduce_scalar(scalar_from_bytes(raw));
    if (is_zero(secret)) {
        return false;
    }
    scalar_to_bytes(secret, raw);
    const bool stored = key_.assign({reinterpret_cast<const char*>(raw.data()), raw.size()}).has_value();
    OPENSSL_cleanse(raw.data(), raw.size());
    if (!stored) {
        return false;
    }
    public_ = to_digest(base_mul(secret));
    loaded_ = true;
    return true;
}

Signature ApiKey::sign(const Digest& digest) const noexcept {
    const auto view = key_.view();
    const auto* key_bytes = reinterpret_cast<const std::uint8_t*>(view.data());
    std::array<std::uint8_t, k_digest_bytes * 2> seed{};
    std::memcpy(seed.data(), key_bytes, k_digest_bytes);
    std::memcpy(seed.data() + k_digest_bytes, digest.data(), k_digest_bytes);
    std::array<std::uint8_t, SHA512_DIGEST_LENGTH> wide{};
    SHA512(seed.data(), seed.size(), wide.data());
    const Scalar nonce = reduce_wide(wide);
    const auto out = schnorr(scalar_from_bytes({key_bytes, k_digest_bytes}), digest, nonce);
    OPENSSL_cleanse(seed.data(), seed.size());
    OPENSSL_cleanse(wide.data(), wide.size());
    return out;
}

} // namespace raikiri::venues::lighter
