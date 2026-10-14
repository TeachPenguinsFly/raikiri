#include <raikiri/venues/apex/zk.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace raikiri::venues::apex::zk {

namespace {

using Limbs = std::array<std::uint64_t, 4>;

constexpr std::uint64_t k_max_u32 = 0xFFFFFFFFULL;
constexpr std::uint64_t k_max_u64 = ~std::uint64_t{0};

[[nodiscard]] constexpr bool less(const Limbs& a, const Limbs& b) noexcept {
    for (std::size_t i = 4; i-- > 0;) {
        if (a[i] != b[i]) {
            return a[i] < b[i];
        }
    }
    return false;
}

constexpr std::uint64_t add_into(Limbs& a, const Limbs& b) noexcept {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const uint128_t sum = static_cast<uint128_t>(a[i]) + b[i] + carry;
        a[i] = static_cast<std::uint64_t>(sum);
        carry = static_cast<std::uint64_t>(sum >> 64);
    }
    return carry;
}

constexpr std::uint64_t sub_into(Limbs& a, const Limbs& b) noexcept {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const uint128_t diff = static_cast<uint128_t>(a[i]) - b[i] - borrow;
        a[i] = static_cast<std::uint64_t>(diff);
        borrow = static_cast<std::uint64_t>(diff >> 64) != 0 ? 1 : 0;
    }
    return borrow;
}

[[nodiscard]] constexpr Limbs double_mod(const Limbs& a, const Limbs& p) noexcept {
    Limbs out = a;
    if (add_into(out, a) != 0 || !less(out, p)) {
        (void)sub_into(out, p);
    }
    return out;
}

struct Fe {
    Limbs v{};
};

// montgomery arithmetic modulo an odd prime below 2^255
class Field {
public:
    explicit constexpr Field(const Limbs& modulus) noexcept : p_{modulus} {
        std::uint64_t x = 1;
        for (int i = 0; i < 6; ++i) {
            x *= 2 - p_[0] * x;
        }
        inv_ = 0 - x;
        Limbs v{1, 0, 0, 0};
        for (int i = 0; i < 256; ++i) {
            v = double_mod(v, p_);
        }
        one_ = v;
        for (int i = 0; i < 256; ++i) {
            v = double_mod(v, p_);
        }
        r2_ = v;
        p_minus_2_ = p_;
        p_minus_2_[0] -= 2;
    }

    [[nodiscard]] const Limbs& modulus() const noexcept { return p_; }

    [[nodiscard]] Fe one() const noexcept { return Fe{one_}; }

    // `canonical` must already be below the modulus
    [[nodiscard]] Fe from(const Limbs& canonical) const noexcept { return mul(Fe{canonical}, Fe{r2_}); }

    [[nodiscard]] Limbs to(const Fe& a) const noexcept { return mul(a, Fe{{1, 0, 0, 0}}).v; }

    [[nodiscard]] Fe add(const Fe& a, const Fe& b) const noexcept {
        Fe out = a;
        if (add_into(out.v, b.v) != 0 || !less(out.v, p_)) {
            (void)sub_into(out.v, p_);
        }
        return out;
    }

    [[nodiscard]] Fe sub(const Fe& a, const Fe& b) const noexcept {
        Fe out = a;
        if (sub_into(out.v, b.v) != 0) {
            (void)add_into(out.v, p_);
        }
        return out;
    }

    [[nodiscard]] Fe mul(const Fe& a, const Fe& b) const noexcept {
        std::array<std::uint64_t, 6> t{};
        for (std::size_t i = 0; i < 4; ++i) {
            uint128_t carry = 0;
            for (std::size_t j = 0; j < 4; ++j) {
                const uint128_t s = static_cast<uint128_t>(a.v[j]) * b.v[i] + t[j] + carry;
                t[j] = static_cast<std::uint64_t>(s);
                carry = s >> 64;
            }
            uint128_t s = static_cast<uint128_t>(t[4]) + carry;
            t[4] = static_cast<std::uint64_t>(s);
            t[5] = static_cast<std::uint64_t>(s >> 64);
            const std::uint64_t q = t[0] * inv_;
            carry = (static_cast<uint128_t>(q) * p_[0] + t[0]) >> 64;
            for (std::size_t j = 1; j < 4; ++j) {
                s = static_cast<uint128_t>(q) * p_[j] + t[j] + carry;
                t[j - 1] = static_cast<std::uint64_t>(s);
                carry = s >> 64;
            }
            s = static_cast<uint128_t>(t[4]) + carry;
            t[3] = static_cast<std::uint64_t>(s);
            t[4] = t[5] + static_cast<std::uint64_t>(s >> 64);
        }
        Fe out{{t[0], t[1], t[2], t[3]}};
        if (t[4] != 0 || !less(out.v, p_)) {
            (void)sub_into(out.v, p_);
        }
        return out;
    }

    [[nodiscard]] Fe pow(const Fe& base, const Limbs& exponent) const noexcept {
        Fe out = one();
        for (std::size_t bit = 256; bit-- > 0;) {
            out = mul(out, out);
            if (((exponent[bit / 64] >> (bit % 64)) & 1U) != 0) {
                out = mul(out, base);
            }
        }
        return out;
    }

    [[nodiscard]] Fe inverse(const Fe& a) const noexcept { return pow(a, p_minus_2_); }

private:
    Limbs p_{};
    Limbs one_{};
    Limbs r2_{};
    Limbs p_minus_2_{};
    std::uint64_t inv_{0};
};

constexpr Field k_fr{{0x43e1f593f0000001ULL, 0x2833e84879b97091ULL, 0xb85045b68181585dULL, 0x30644e72e131a029ULL}};
constexpr Field k_fs{{0x677297dc392126f1ULL, 0xab3eedb83920ee0aULL, 0x370a08b6d0302b0bULL, 0x060c89ce5c263405ULL}};

// 5^-1 modulo r - 1
constexpr Limbs k_alpha_inv{0xcfe7f7a98ccccccdULL, 0x535cb9d394945a0dULL, 0x93736af8679aad17ULL, 0x26b6a528b427b354ULL};
constexpr Limbs k_edwards_d{0xd075ca8cf4d7eb8eULL, 0x039b2959ebb7c867ULL, 0x3df072d799fd11fcULL, 0x1aee90f15f218969ULL};
constexpr Limbs k_generator_x{
    0xfd221dc052d76df7ULL, 0x854a1c1c06cd5cc8ULL, 0x4e9803958f6c320eULL, 0x2ef3f9b423a2c8c7ULL};
constexpr Limbs k_generator_y{
    0x984b5c0e9c6c94b7ULL, 0x7532c815f5f6d57dULL, 0x784224644a68e406ULL, 0x05a01167ea785d3fULL};

constexpr std::size_t k_rescue_width = 3;
constexpr std::size_t k_rescue_rounds = 22;

// the 2-into-1 bn256 parameters: blake2s "Rescue_f" round constants and the "ResM0003" mds matrix
constexpr std::array<Limbs, 135> k_round_constants{{
    {0x97216a720f7b48acULL, 0xfba992c691596d3eULL, 0x44c8e94bc429a6b9ULL, 0x2e827c42545887ffULL},
    {0x43257c6dc6e54dcaULL, 0xd06953671bbbf7deULL, 0xaf0d7fa782a1c0b7ULL, 0x03c75f89abe4ca0bULL},
    {0x37c734be0df06cc0ULL, 0x4bba886dec261746ULL, 0x567f7362d1c8e62dULL, 0x19774ac9566bf383ULL},
    {0x40cf341f0be88470ULL, 0x4b52084db7832ba7ULL, 0xcba7666e4e024bd2ULL, 0x043d62d812dac074ULL},
    {0x4d222847faf89051ULL, 0x49286bf95a0493afULL, 0x4e86b5ab59b734dfULL, 0x04e8b099362f0cbfULL},
    {0xdeb7935e9229ef94ULL, 0xfc8387c3ba93baabULL, 0x97e1bf3c92c66869ULL, 0x07e7156a711c0dceULL},
    {0x0bb7cbe1317dc565ULL, 0xff151b61e1c717d5ULL, 0xefea1ab6c2e25773ULL, 0x1d4418eb293b45bcULL},
    {0x50bce1a4d10a525cULL, 0xcdebf16c08585978ULL, 0xc0483b06b6a17debULL, 0x02cf0956c2566ed0ULL},
    {0xb33ff8ead7ded122ULL, 0x93ab4e10559a901fULL, 0xd212b1450943445aULL, 0x0faa7ae09629bdacULL},
    {0x2afa39189b8eebc9ULL, 0x9f0cb13d16cea22cULL, 0xccebdb8e7127dfa6ULL, 0x1020d9f9e31ac33aULL},
    {0x355beff21a91d184ULL, 0xd0d13fb983e3a6feULL, 0xc19b413a6882897dULL, 0x0a686243a889e0adULL},
    {0x16f8eebe854ef9ebULL, 0x68390d6aacee13e6ULL, 0x756872d4c7141247ULL, 0x1c83e8b36d64cc64ULL},
    {0x9a1d94bf8575f418ULL, 0x8d3eefd1015a7adbULL, 0xc0e4612d12d96b27ULL, 0x2c24954fe8789a1dULL},
    {0xcb287728eea9fb09ULL, 0xfaf068a8324713b8ULL, 0x5c2bc5cf6d4dfccbULL, 0x113b0d59035669b6ULL},
    {0x3d01ae41f1201fa8ULL, 0xc24a01410747a595ULL, 0xd190473db97c8129ULL, 0x1eda7697977e5ff6ULL},
    {0xdd5cef1e31a457eeULL, 0x92db494e8ee4cd1cULL, 0x244edba9a6028179ULL, 0x06336fa6da86fea1ULL},
    {0x5a97feb851e923afULL, 0xc74f3f8f07744727ULL, 0x8b5f7dde5c2d9a46ULL, 0x1cbb120a8d2384f9ULL},
    {0xffbe5e3b4113c475ULL, 0x6a78de6fc8706045ULL, 0x1103d182e196c91fULL, 0x292a5b37425d93e0ULL},
    {0xa923d38611b2ba51ULL, 0x8d5fe971f8fe4b04ULL, 0x83d0d6734d4eb1a2ULL, 0x147729a654c70dfdULL},
    {0x2c04375882981f74ULL, 0xb5c86f8c553b9243ULL, 0xae841646c8c8af01ULL, 0x11c91cdf81970c31ULL},
    {0x2041a2fa49802fd6ULL, 0x3a0489121a83da53ULL, 0x95fab90c3bda39f9ULL, 0x0833bdaf9311d54bULL},
    {0x99cd820a4eec27f8ULL, 0xcbd5dfe3c9d9eee6ULL, 0x52667494f19b36bcULL, 0x24c823de84f3c920ULL},
    {0xbf193d370664d429ULL, 0x7b08d6ea24850406ULL, 0xcfcada03edba08a8ULL, 0x2d5e9c028e0d90fbULL},
    {0x3baec1956b7bdc69ULL, 0x9f7900eb83de841aULL, 0x9d54f9587e87736fULL, 0x262a0fb1764ca2a6ULL},
    {0xf4380ea05a01d3feULL, 0xda4211826fd5442bULL, 0x8876809e847b8412ULL, 0x303b15e35ddb721bULL},
    {0xced542d618cc877fULL, 0x84c135210cc7ba2aULL, 0xfcd68b0ccc952638ULL, 0x0dad6859822f688dULL},
    {0x45c53a1e44c822e0ULL, 0xf8938eed24d309eaULL, 0x25d6e57d1aca4015ULL, 0x20fb16de97d6fe90ULL},
    {0xebc8d39383d6bd99ULL, 0x312cc0fd813a2158ULL, 0x5efb5f4610aea2f6ULL, 0x290804f3f5d08d17ULL},
    {0x77551ed9a396d271ULL, 0x72d6cb908e35f389ULL, 0xbed8b6c03136354fULL, 0x14182199a12b196fULL},
    {0x77032770d188ead5ULL, 0xbfffe29ac83fbed4ULL, 0x224b41ca808f5213ULL, 0x2c23201a5e1e868bULL},
    {0x29cbdbf7803e9291ULL, 0x57c728e7b0d65503ULL, 0x3aa145b52f72f6e5ULL, 0x0fa4cadd5eb82dacULL},
    {0x828fc9c8f7d57d3aULL, 0xfd349fef82883c4cULL, 0x551d8d8149018abcULL, 0x0aa69f987c5d7d59ULL},
    {0x2f45bb5c5a9ea2a7ULL, 0x602656e8f4d201d0ULL, 0x2f0f7b8fc31bf22eULL, 0x2ae7aeee1710fac6ULL},
    {0x4391d65432525a47ULL, 0x1a0283488eaa3253ULL, 0x18a7caede8d58ff0ULL, 0x17efbf28294d5f63ULL},
    {0x0c98771efc01b36eULL, 0x58b013fd3a2c3ec7ULL, 0xf8bc16e68936c054ULL, 0x07d52d080d752529ULL},
    {0x30187e28036b96e9ULL, 0x0eb222bb8ebbe4daULL, 0x0d1a795b1918a770ULL, 0x09323e13b19f985dULL},
    {0x3bb84fde2bb12cb2ULL, 0x48c1b678f51d0b3aULL, 0x097a5f45814cd091ULL, 0x241e50fb623a7713ULL},
    {0xf3a2062b6c6fb861ULL, 0x6630e154bd0358a5ULL, 0x7ba66e5fd9bef4eeULL, 0x2ab7da957ae22343ULL},
    {0xd1385a6b2691ecabULL, 0x7be69b8d369ea861ULL, 0xfb3cdab49464c107ULL, 0x128bc77708c998f8ULL},
    {0xe15575e6e1b3c5b4ULL, 0xd779d91b1ee62012ULL, 0x9971a88243ef7a0eULL, 0x179ee05447dcf208ULL},
    {0x1e9bafe3bc1220a9ULL, 0xbbf056cf3c68f2ccULL, 0x13b20f90540f25a7ULL, 0x2b14779395dabf0eULL},
    {0x28d5fedd3dec311cULL, 0x1b3c9b21ea578848ULL, 0xaf7f58bf923f8a13ULL, 0x27b3090f24d3ec29ULL},
    {0xc09e56e23348f98fULL, 0xeb83d1cdc0d3493fULL, 0x783a1fc07048fce9ULL, 0x219b7eb539cd8f5eULL},
    {0x79cb2e5a3e2da771ULL, 0x11da4b7932d5a9d7ULL, 0x9b8a0836ceff2c7fULL, 0x075941e54effafe2ULL},
    {0xa8ac4c5e937fe621ULL, 0x6b40906465a8c1a1ULL, 0x268477b3c246848dULL, 0x13db5be2a23de171ULL},
    {0x5dd0d387e8cebe44ULL, 0xf423f8df34d5efc8ULL, 0x5b882b7982faf2b8ULL, 0x086ffc0ed3c8a41bULL},
    {0xd8a2dd6b29b5306eULL, 0x6bbc982784c9f3a3ULL, 0x400ac145ca8062a8ULL, 0x0bae52cf7553fbcbULL},
    {0x1b09c3c9dbda5c12ULL, 0x574ab7be3c3034b6ULL, 0x4764dbaa71ecb67aULL, 0x0c5d0c9822e8064bULL},
    {0x9e4d8bb0c2719809ULL, 0xd9d1d82fbd47bcdeULL, 0xb4548778edb2ff17ULL, 0x09324dbb4fa19d86ULL},
    {0xaca220cadb16fcb0ULL, 0x48649f6ae8898f7eULL, 0x2c86a8400eb318eaULL, 0x241d32c2e6a3c0d6ULL},
    {0x1ba125d963ab2482ULL, 0x7120e5f714df7fc9ULL, 0x03155b6ab0bc2dd9ULL, 0x03b43fcc68db6c03ULL},
    {0x78fe4e977b9aee8dULL, 0x7107e89d2dae8b0eULL, 0x55131c98a26a277fULL, 0x0a8381ff49e27157ULL},
    {0xe5d34827bfe0b2adULL, 0x3d620d64dfbbdc7cULL, 0xaa207413a79a6793ULL, 0x249a4becebe8221bULL},
    {0xd4a469f8c651ea2aULL, 0x39a85226fb82c7ecULL, 0x48deb10cc96f1e0eULL, 0x16bb4dafd6d3a0adULL},
    {0x4221d661efaa3b8aULL, 0x124cf54c7e301dabULL, 0xfbfdfb35331a243eULL, 0x2571b13b8139341cULL},
    {0xe5f97307b7a3f66dULL, 0xf84b6d786527f842ULL, 0xdbebadb200384adfULL, 0x1f082b11fd4514d0ULL},
    {0xa5229918c5bef10aULL, 0x732a6a776a570673ULL, 0x56219c6d450a88e9ULL, 0x12b36359c0b3a846ULL},
    {0x0c6df50759afb82fULL, 0xd45c042ad4f0d9e6ULL, 0xd5181d3e2d761f7fULL, 0x22fa7160d8e1135bULL},
    {0x522e1b45384d8c70ULL, 0xaed9600e8ba037bfULL, 0x059e88c6bb9d4489ULL, 0x1c3fba7f717ffa81ULL},
    {0x5da02ece31040d88ULL, 0x676ac05614df9b17ULL, 0xf2fc6d8f2d508a06ULL, 0x1c56fd26e55afa44ULL},
    {0xfa51ff1bd174b475ULL, 0xb70629c22c213c91ULL, 0x4b9e857c5d2f746aULL, 0x1df86d2ebe9bd907ULL},
    {0x13255fe4c43df747ULL, 0xf8db2124486e260fULL, 0xe1508406a11515d2ULL, 0x0c3cee4e1a0d78b7ULL},
    {0x669e767239575ffcULL, 0x547cec2fb50ac7cfULL, 0x072c4a1f15c5d417ULL, 0x2067e59f65bbdd4fULL},
    {0xce24796873590e68ULL, 0xa2711b4e29e1ee82ULL, 0x4270b59c29a5f894ULL, 0x2ef209879e17db4bULL},
    {0x4fb7a5cc4d947eb4ULL, 0x6c99736442c34baeULL, 0x05be861c58972b78ULL, 0x274fd928d6080debULL},
    {0x6389ca890891c913ULL, 0xe958b09e921dd03eULL, 0x9640e6760af80b50ULL, 0x11979c252ae6ac6cULL},
    {0x93f2eba170ef54e0ULL, 0x1a9ead5f52b497c7ULL, 0xed30a3f2b6c9e030ULL, 0x267082663cdcd184ULL},
    {0xa20d19be17981d43ULL, 0x19b841e3471b9b80ULL, 0x7be170fecf619dedULL, 0x2d0667da81aea523ULL},
    {0xac71f518ab79a617ULL, 0x28ea90eacdfc0df2ULL, 0xa53a180d6fb8d7c0ULL, 0x2815464fcc9566a3ULL},
    {0x5d0a2f0e1130deb2ULL, 0x4fc206d27b4d2ee3ULL, 0xf9f760e74c693074ULL, 0x0bd5a8700ca30de6ULL},
    {0x9098f28e8899bb23ULL, 0x628c3ebfb2098f4bULL, 0xbbd44ee961ed0ddaULL, 0x18a10c4f55b6311dULL},
    {0xd95529ac4ada28bfULL, 0xb7f88ccbcbe852f8ULL, 0xf1b61e47dc04124dULL, 0x28a57a4a66f72150ULL},
    {0xfeb24196e64e6507ULL, 0x8824d2ddf9fb3432ULL, 0xeb86b55a1754d897ULL, 0x160e287a5ab0869fULL},
    {0x47334ebb6afbca12ULL, 0xa11bd7a01eaf65ccULL, 0xc4f2e882ec0a52c0ULL, 0x1f8d9bc1be9bb169ULL},
    {0x968cfd1a854a302cULL, 0xcb4d6a636cf2158eULL, 0x92295ed3b56bef36ULL, 0x29c392cab71cfb96ULL},
    {0xe511dbe0f492293cULL, 0x61ee66bcb468a667ULL, 0x443881d104fdde81ULL, 0x0cf1c91a22e683c1ULL},
    {0x6dbbcafb211069c7ULL, 0xe31555d4f504e9a0ULL, 0x3d9f6926340abdf1ULL, 0x2ae4874d96c7c84aULL},
    {0x54f90164db4a6572ULL, 0xbe8e20d512d258c0ULL, 0x5d2d12674834ae08ULL, 0x02b0ad5c08cb1039ULL},
    {0x496bf1acedd86110ULL, 0x22dfcf8fc5a425b7ULL, 0x4335749abebdc9edULL, 0x0a497b74786056baULL},
    {0xc43ab0fb5d3d855bULL, 0x620d5f9152d5d524ULL, 0xead3e510ca0855c1ULL, 0x29d3c40517092e92ULL},
    {0x93b508544b13c7f5ULL, 0x06ff5265952071f8ULL, 0x3e3d7ab586c09980ULL, 0x1205185af96147aaULL},
    {0x4f97a8319515191eULL, 0x46755f161bc91476ULL, 0x08c0624e279c9b4aULL, 0x10cfeadc8cd3196aULL},
    {0x9dba0f63d64d4f04ULL, 0x4856f607df91603bULL, 0xa9296d9172207a64ULL, 0x27792e3c522f9f78ULL},
    {0x009b2637ba7e95baULL, 0x075d7ba2f1199ac1ULL, 0x7fccd06f4f2b8e1cULL, 0x276c221946ed5924ULL},
    {0x95935933e2c145c3ULL, 0x12474e29195ddb29ULL, 0x5e467417682e6040ULL, 0x07f97f0b29d227dcULL},
    {0x967301514560af54ULL, 0x06acf7aa488b9457ULL, 0x8a533932ad49da94ULL, 0x23999e25e79086edULL},
    {0x94f8b670642f7ab7ULL, 0x897d74d8cfe000e0ULL, 0x1bcedbb718e6da69ULL, 0x23c70014ae1555c3ULL},
    {0x032b07880433a60dULL, 0x4449a21c18ffefd9ULL, 0x0c6f66621bbd9f8bULL, 0x2af92d6b81674d25ULL},
    {0xe318b093da5b4984ULL, 0xe8586c7ce0a50ab7ULL, 0x595d16ea11cc2827ULL, 0x1baabd1b5e38855fULL},
    {0x364512646346ad82ULL, 0x1c96ad68b7799931ULL, 0xe58a897daf48902bULL, 0x2d49d4ed96f91184ULL},
    {0x6c5d3f576a46046aULL, 0x48d8bf97419453c2ULL, 0x0f4214df7d6fd800ULL, 0x0241072649eb7fc6ULL},
    {0x91a284362da953f5ULL, 0x80080fadb544369cULL, 0x8d475eb142453935ULL, 0x12adde3c8f441148ULL},
    {0x8d2eef43342f9077ULL, 0x3f6b29c555257280ULL, 0xe5d783d78ef557acULL, 0x2d9ce38e088cc3d8ULL},
    {0x2dca86dfb1769085ULL, 0xccdbf39e527d1964ULL, 0x02a77c6a6268ffd7ULL, 0x1d96452671994e16ULL},
    {0x8c68318cf351df2dULL, 0x6db35163021169b0ULL, 0xdae14375559290f3ULL, 0x01da79fbce33ec6dULL},
    {0xa4bb66bb89d7efceULL, 0xfb7e7957afd8c54bULL, 0x778f7d40335fb169ULL, 0x2b9589381bf636caULL},
    {0xb822db035f34161cULL, 0x063d75aee95f4b9aULL, 0xbc3fbccdc3da2abcULL, 0x15438d653cefa32fULL},
    {0x19e8689c6cb2d742ULL, 0x28e8556b2d4bb0c7ULL, 0x30820ac0a75846c8ULL, 0x11a38e9b2f0a2f82ULL},
    {0x5f7cca82fe1bcccfULL, 0xcea826c250b75f31ULL, 0x40c2039f5278de49ULL, 0x20aebd8eaa2ba10cULL},
    {0x0aec0e609cdddbb7ULL, 0x3bee2eb6f1971936ULL, 0xf214effe5faa7f9aULL, 0x2805f77ba0f90bfbULL},
    {0xd7c98703f632b7f1ULL, 0x14d524aa6274bf7dULL, 0xbdfb7aecc3a471d0ULL, 0x1f35fd366f894caeULL},
    {0xa18d268ff3e364bdULL, 0xed927c29716e8a23ULL, 0x4d764b2d8126f9ecULL, 0x1138a44eb81c2dedULL},
    {0xa97dea7792068575ULL, 0x7613f90d39f6e4b7ULL, 0x1033488bdb59d17eULL, 0x09ea16c15205adf6ULL},
    {0x9fcac2acf123ad5aULL, 0xf97ac8e9d28a8fecULL, 0xd1682e8a5da2e030ULL, 0x18642ea66de5b1efULL},
    {0xb433e078cf806ce9ULL, 0x780034e5446db453ULL, 0x05df5d853097ea0aULL, 0x2d00b31b568c5b73ULL},
    {0x3c1f12e0e4276ab0ULL, 0x3ac0fe765e18d1c8ULL, 0x40459e321270221cULL, 0x0bdbdb7dbc5f742eULL},
    {0x9ccbe21ffeec9685ULL, 0x1c3fa169bee22883ULL, 0xa6be2bce5e1e395aULL, 0x27f0f1e9106b61d0ULL},
    {0xbd73b94454058983ULL, 0xd314bf5c9be58315ULL, 0x9ad62e3b6faba355ULL, 0x19a6c9fc9430f3f3ULL},
    {0x3d18baf5cd2913b3ULL, 0x7604b17937f1b24dULL, 0xbdabf94450b5681fULL, 0x0117d38e94416d29ULL},
    {0xcd7b74924e4fcbe5ULL, 0xbe27bc1c4107a602ULL, 0xb6bab41cbf3ba2ddULL, 0x0561af313be692a3ULL},
    {0x76e83983bc7ffd51ULL, 0x8c23e1b13fb53595ULL, 0x33d3bf82f29eda7cULL, 0x17e0bf6748e45915ULL},
    {0x3e4a60e52d77e2bdULL, 0xb971ff87fd5f8b7cULL, 0xfb22a83575e37310ULL, 0x20301abe834d1352ULL},
    {0x131378dbd2e554ceULL, 0x86bc97ef1f595cffULL, 0xf34fe68d6912d282ULL, 0x2af84be1a167caccULL},
    {0x61cad2561c40306fULL, 0x9891790098888b09ULL, 0x868773b89838cc89ULL, 0x05ea371f188b0d76ULL},
    {0x6f4ceac1e4c67842ULL, 0xaee4924adbe82728ULL, 0x39fb4421ad10b683ULL, 0x0cf6ce4d9373be95ULL},
    {0xb602ff31b48ce6a3ULL, 0xf2b16ba89976521eULL, 0xd86dff873e22f32dULL, 0x0aca25eb78a73109ULL},
    {0x199d8b7642e5234fULL, 0x9907a6664bfeea10ULL, 0x5d0b03956cf32d49ULL, 0x1377ac6a61a13e8bULL},
    {0x628455025e36643aULL, 0x893a39896dd85c08ULL, 0xc1a4a3b0359c1ffdULL, 0x0117c5b7fed49913ULL},
    {0x62954a15d1e4096bULL, 0xe69e06b0db01351cULL, 0x8f74a335bebaef3aULL, 0x1f51935bcdc7c51bULL},
    {0x24fd426b9ec131d7ULL, 0x3ce95085a67b2fc5ULL, 0xc4221c3527563f33ULL, 0x2817ee19e12e7566ULL},
    {0x02389d974e1c4373ULL, 0x7eed276dbb9df45cULL, 0x446cec07dcadb4a0ULL, 0x0959c2acc31d5651ULL},
    {0x24a1f4bb8b09d602ULL, 0xa27ccf79495043dcULL, 0x392f6afa130b34a4ULL, 0x0312c8f6f38ee402ULL},
    {0x6d4e86de1d2b209cULL, 0x1e3b1c4433ccb2e7ULL, 0x41af1ee4dc87a9a5ULL, 0x27cf6996f2db864bULL},
    {0x4e41af45801fe0bbULL, 0xa4e9ef8f8ca2caa5ULL, 0x7bb40a2400f32552ULL, 0x0c9b46ee380ed149ULL},
    {0x5680691966813e99ULL, 0x5e23f1e5bf774f97ULL, 0xaead79cc75d304f1ULL, 0x25c787129ae25108ULL},
    {0x346529120907f99fULL, 0x5f546c3db83a3b3dULL, 0x8c5d8437f91432beULL, 0x0e1982f2eaff6732ULL},
    {0x0123dffe86f6e950ULL, 0x29ca975972804684ULL, 0xb9bfa20495856230ULL, 0x2d3d5fc23723cf7eULL},
    {0xca10acc3c4c22351ULL, 0x2486c5273b2ae704ULL, 0x215e89b77b6d51d7ULL, 0x06befe53a900cdccULL},
    {0x5cdf7300c90e8e69ULL, 0x5749c1c6934444e7ULL, 0xa5938f4346683faeULL, 0x0345abcbd6380b14ULL},
    {0x61c36fe53a549beeULL, 0x729bc9a0a94508d2ULL, 0xd507d26e2789d095ULL, 0x04826ffdef990240ULL},
    {0xd988ac99c94178fcULL, 0xd2b0b4b0775bfce0ULL, 0x996f6bdf2d2ec503ULL, 0x1d6dfc9dcf49c371ULL},
    {0xb2f9d8342d647b52ULL, 0x4cd54b54cbdd4fb2ULL, 0x245ef2f98b2b1adfULL, 0x184d027e3c7bdb1fULL},
    {0x12376048d61650deULL, 0x455e0a93155a0eecULL, 0xb3360b737d446985ULL, 0x2062808e98d02369ULL},
    {0xf949abde45b9b646ULL, 0xe70c66deafe51b0bULL, 0x41d66683572814b1ULL, 0x2de847a34da34f6cULL},
    {0x8a7cfdc1ba3d7645ULL, 0x3b86aed752291492ULL, 0xf715619f80da6293ULL, 0x164ed456b62365fbULL},
}};
constexpr std::array<Limbs, 9> k_mds{{
    {0xc0bd8c37d1c6b3e3ULL, 0xc8be8189ccea49a4ULL, 0xd9c72a3d8c4d363aULL, 0x2bec9c48301bdaaaULL},
    {0x9a1af9e21f5121a8ULL, 0xa34de4c88515d84cULL, 0x62469e5f812fe7acULL, 0x1f069c8a0ff22a1eULL},
    {0xcea4ba91e4537c0bULL, 0xfa94d552d01f19e0ULL, 0xecb069c1a5b6f3feULL, 0x0d5365c702a1d156ULL},
    {0xd4548a366740959eULL, 0x5e5599737bfa3c30ULL, 0x47e951763747a13fULL, 0x1e4038a58dfdab61ULL},
    {0xf96649f53f7824e3ULL, 0x49e98b266a981933ULL, 0xa52e08fa94164588ULL, 0x07cf3d4093243419ULL},
    {0xde19589968b8d95aULL, 0xe65187597edb3d37ULL, 0x020f035edc4ec2b5ULL, 0x116b9e8295f0086eULL},
    {0xfc804813c15542fbULL, 0x2d3b80bff560df8aULL, 0xa4ef91c0f455ebdaULL, 0x1e2a87b6b95d6c53ULL},
    {0xa6986e51cfe3a96bULL, 0xe611b03897f65996ULL, 0xe9c30527df038594ULL, 0x16e10361cbf35fe8ULL},
    {0xcc4c713c4137c980ULL, 0x117666918dae1210ULL, 0x83849539f556b020ULL, 0x1c794b13d4b66d98ULL},
}};

using State = std::array<Fe, k_rescue_width>;

struct RescueTables {
    std::array<Fe, k_round_constants.size()> constants{};
    std::array<Fe, k_mds.size()> mds{};
};

[[nodiscard]] const RescueTables& rescue_tables() noexcept {
    static const RescueTables tables = [] {
        RescueTables out{};
        for (std::size_t i = 0; i < k_round_constants.size(); ++i) {
            out.constants[i] = k_fr.from(k_round_constants[i]);
        }
        for (std::size_t i = 0; i < k_mds.size(); ++i) {
            out.mds[i] = k_fr.from(k_mds[i]);
        }
        return out;
    }();
    return tables;
}

void rescue_permute(State& state) noexcept {
    const auto& tables = rescue_tables();
    for (std::size_t i = 0; i < k_rescue_width; ++i) {
        state[i] = k_fr.add(state[i], tables.constants[i]);
    }
    for (std::size_t round = 0; round < 2 * k_rescue_rounds; ++round) {
        for (auto& element : state) {
            if (round % 2 == 0) {
                element = k_fr.pow(element, k_alpha_inv);
            } else {
                const auto square = k_fr.mul(element, element);
                element = k_fr.mul(element, k_fr.mul(square, square));
            }
        }
        State next{};
        for (std::size_t row = 0; row < k_rescue_width; ++row) {
            next[row] = tables.constants[(round + 1) * k_rescue_width + row];
            for (std::size_t col = 0; col < k_rescue_width; ++col) {
                next[row] = k_fr.add(next[row], k_fr.mul(state[col], tables.mds[row * k_rescue_width + col]));
            }
        }
        state = next;
    }
}

constexpr std::size_t k_fr_capacity = 253;
constexpr std::size_t k_fs_capacity = 250;
constexpr std::size_t k_max_bits = 768;

// bytes read most significant bit first, then cut into 253-bit field elements weighted from their first bit
class BitPacker {
public:
    void append(std::span<const std::uint8_t> bytes) noexcept {
        for (const auto byte : bytes) {
            for (int bit = 7; bit >= 0; --bit) {
                bits_[count_++] = static_cast<std::uint8_t>((byte >> bit) & 1U);
            }
        }
    }

    void pad_to(std::size_t count) noexcept { count_ = count > count_ ? count : count_; }

    [[nodiscard]] std::size_t pack(std::array<Fe, 4>& out) const noexcept {
        std::size_t elements = 0;
        for (std::size_t start = 0; start < count_; start += k_fr_capacity) {
            Limbs value{};
            const std::size_t end = start + k_fr_capacity < count_ ? start + k_fr_capacity : count_;
            for (std::size_t i = start; i < end; ++i) {
                if (bits_[i] != 0) {
                    const std::size_t at = i - start;
                    value[at / 64] |= std::uint64_t{1} << (at % 64);
                }
            }
            out[elements++] = k_fr.from(value);
        }
        return elements;
    }

private:
    std::array<std::uint8_t, k_max_bits> bits_{};
    std::size_t count_{0};
};

[[nodiscard]] Bytes32 le_bytes(const Limbs& value) noexcept {
    Bytes32 out{};
    for (std::size_t i = 0; i < 32; ++i) {
        out[i] = static_cast<std::uint8_t>(value[i / 8] >> (8 * (i % 8)));
    }
    return out;
}

[[nodiscard]] Bytes32 be_bytes(const Limbs& value) noexcept {
    Bytes32 out{};
    for (std::size_t i = 0; i < 32; ++i) {
        out[31 - i] = static_cast<std::uint8_t>(value[i / 8] >> (8 * (i % 8)));
    }
    return out;
}

[[nodiscard]] Limbs read_be(std::span<const std::uint8_t, 32> bytes) noexcept {
    Limbs out{};
    for (std::size_t i = 0; i < 32; ++i) {
        out[i / 8] |= static_cast<std::uint64_t>(bytes[31 - i]) << (8 * (i % 8));
    }
    return out;
}

[[nodiscard]] Limbs low_bits(const Limbs& value, std::size_t count) noexcept {
    Limbs out{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (count >= 64 * (i + 1)) {
            out[i] = value[i];
        } else if (count > 64 * i) {
            out[i] = value[i] & ((std::uint64_t{1} << (count - 64 * i)) - 1);
        }
    }
    return out;
}

// the sdk's rescue_hash_to_scalar: half the capacity from each of two squeezes
[[nodiscard]] Limbs rescue_h_star(const Bytes32& pk_x, const Bytes32& r_x, const Bytes32& message) noexcept {
    BitPacker bits;
    bits.append(pk_x);
    bits.append(r_x);
    bits.append(message);
    std::array<Fe, 4> inputs{};
    (void)bits.pack(inputs);

    State state{};
    state[2] = k_fr.from({4, 0, 0, 0});
    state[0] = k_fr.add(state[0], inputs[0]);
    state[1] = k_fr.add(state[1], inputs[1]);
    rescue_permute(state);
    state[0] = k_fr.add(state[0], inputs[2]);
    state[1] = k_fr.add(state[1], inputs[3]);
    rescue_permute(state);

    constexpr std::size_t half = k_fs_capacity / 2;
    const auto first = low_bits(k_fr.to(state[0]), half);
    const auto second = low_bits(k_fr.to(state[1]), half);
    Limbs out = first;
    for (std::size_t i = 0; i < half; ++i) {
        if (((second[i / 64] >> (i % 64)) & 1U) != 0) {
            const std::size_t at = i + half;
            out[at / 64] |= std::uint64_t{1} << (at % 64);
        }
    }
    return out;
}

struct Point {
    Fe x{};
    Fe y{};
    Fe z{};
    Fe t{};
};

// twisted edwards with a = -1; the unified formula also doubles
[[nodiscard]] Point add(const Point& p, const Point& q) noexcept {
    static const Fe two_d = [] {
        const auto d = k_fr.from(k_edwards_d);
        return k_fr.add(d, d);
    }();
    const auto a = k_fr.mul(k_fr.sub(p.y, p.x), k_fr.sub(q.y, q.x));
    const auto b = k_fr.mul(k_fr.add(p.y, p.x), k_fr.add(q.y, q.x));
    const auto c = k_fr.mul(k_fr.mul(p.t, two_d), q.t);
    const auto zz = k_fr.mul(p.z, q.z);
    const auto d = k_fr.add(zz, zz);
    const auto e = k_fr.sub(b, a);
    const auto f = k_fr.sub(d, c);
    const auto g = k_fr.add(d, c);
    const auto h = k_fr.add(b, a);
    return Point{.x = k_fr.mul(e, f), .y = k_fr.mul(g, h), .z = k_fr.mul(f, g), .t = k_fr.mul(e, h)};
}

[[nodiscard]] Point base_mul(const Limbs& scalar) noexcept {
    static const Point generator = [] {
        const auto x = k_fr.from(k_generator_x);
        const auto y = k_fr.from(k_generator_y);
        return Point{.x = x, .y = y, .z = k_fr.one(), .t = k_fr.mul(x, y)};
    }();
    Point out{.x = Fe{}, .y = k_fr.one(), .z = k_fr.one(), .t = Fe{}};
    for (std::size_t bit = 256; bit-- > 0;) {
        out = add(out, out);
        if (((scalar[bit / 64] >> (bit % 64)) & 1U) != 0) {
            out = add(out, generator);
        }
    }
    return out;
}

struct Affine {
    Limbs x{};
    Limbs y{};
};

[[nodiscard]] Affine affine(const Point& p) noexcept {
    const auto z_inv = k_fr.inverse(p.z);
    return Affine{.x = k_fr.to(k_fr.mul(p.x, z_inv)), .y = k_fr.to(k_fr.mul(p.y, z_inv))};
}

// y little-endian with the parity of x in the top bit
[[nodiscard]] Bytes32 pack_point(const Affine& p) noexcept {
    auto out = le_bytes(p.y);
    if ((p.x[0] & 1U) != 0) {
        out[31] |= 0x80;
    }
    return out;
}

[[nodiscard]] Bytes32 sha256(std::span<const std::uint8_t> data) noexcept {
    Bytes32 out{};
    SHA256(data.data(), data.size(), out.data());
    return out;
}

[[nodiscard]] Bytes32 hmac(const Bytes32& key, std::span<const std::uint8_t> data) noexcept {
    Bytes32 out{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), out.data(), &len);
    return out;
}

// the sdk's Seed::deterministic_seed, including its retry, which keys on v || v || 0
[[nodiscard]] Limbs deterministic_nonce(const Limbs& secret, const Bytes32& message) noexcept {
    const auto h1 = sha256(message);
    const auto secret_be = be_bytes(secret);
    Bytes32 v{};
    v.fill(1);
    Bytes32 key{};
    std::array<std::uint8_t, 97> joined{};
    const auto join = [&](std::uint8_t tag) {
        std::memcpy(joined.data(), v.data(), 32);
        joined[32] = tag;
        std::memcpy(joined.data() + 33, secret_be.data(), 32);
        std::memcpy(joined.data() + 65, h1.data(), 32);
    };
    join(0);
    key = hmac(key, joined);
    v = hmac(key, v);
    join(1);
    key = hmac(key, joined);
    v = hmac(key, v);
    std::array<std::uint8_t, 65> retry{};
    for (;;) {
        const auto k = hmac(key, v);
        const auto candidate = read_be(k);
        if (less(candidate, k_fs.modulus())) {
            OPENSSL_cleanse(joined.data(), joined.size());
            return candidate;
        }
        std::memcpy(retry.data(), v.data(), 32);
        std::memcpy(retry.data() + 32, v.data(), 32);
        retry[64] = 0;
        key = hmac(key, retry);
    }
}

[[nodiscard]] int hex_digit(char c) noexcept {
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

// python's int / int: the quotient correctly rounded to a double, then truncated by int()
[[nodiscard]] std::uint64_t float_quotient_truncated(std::uint64_t a, std::uint64_t b) noexcept {
    const uint128_t numerator = static_cast<uint128_t>(a) << 64;
    uint128_t q = numerator / b;
    const bool sticky = numerator % b != 0;
    const auto high = static_cast<std::uint64_t>(q >> 64);
    const int width = high != 0 ? 128 - std::countl_zero(high) : 64 - std::countl_zero(static_cast<std::uint64_t>(q));
    if (width > 53) {
        const int shift = width - 53;
        const uint128_t rest = q & ((uint128_t{1} << shift) - 1);
        const uint128_t half = uint128_t{1} << (shift - 1);
        q >>= shift;
        if (rest > half || (rest == half && (sticky || (q & 1U) != 0))) {
            ++q;
        }
        q <<= shift;
    }
    return static_cast<std::uint64_t>(q >> 64);
}

constexpr std::uint8_t k_contract_msg_type = 0xfe;
constexpr std::uint64_t k_max_mantissa = (std::uint64_t{1} << 35) - 1;

} // namespace

std::array<std::uint8_t, 5> pack_amount(uint128_t amount) noexcept {
    std::uint64_t exponent = 0;
    uint128_t scale = 1;
    while (amount > k_max_mantissa * scale && exponent < 31) {
        scale *= 10;
        ++exponent;
    }
    std::uint64_t mantissa = 0;
    if (exponent == 0) {
        mantissa = static_cast<std::uint64_t>(amount);
    } else {
        mantissa = static_cast<std::uint64_t>(amount / scale);
        const uint128_t below = amount - mantissa * scale;
        const uint128_t capped = amount - k_max_mantissa * scale / 10;
        if (below >= capped) {
            --exponent;
            mantissa = k_max_mantissa;
        }
    }
    const std::uint64_t packed = (mantissa << 5) | exponent;
    std::array<std::uint8_t, 5> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        out[4 - i] = static_cast<std::uint8_t>(packed >> (8 * i));
    }
    return out;
}

std::array<std::uint8_t, k_contract_bytes> contract_bytes(const ContractOrder& order) noexcept {
    std::array<std::uint8_t, k_contract_bytes> out{};
    std::size_t at = 0;
    const auto put = [&](std::uint64_t value, std::size_t width) {
        for (std::size_t i = width; i-- > 0;) {
            out[at++] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    };
    put(k_contract_msg_type, 1);
    put(order.account_id, 4);
    put(order.sub_account_id, 1);
    put(order.slot_id & 0xFFFFU, 2);
    put(order.nonce & 0xFFFFFFU, 3);
    put(order.pair_id & 0xFFU, 1);
    put(order.buy ? 1 : 0, 1);
    for (const auto byte : pack_amount(order.size)) {
        out[at++] = byte;
    }
    put(static_cast<std::uint64_t>(order.price >> 64) & 0xFFFFFFFFFFFFFFULL, 7);
    put(static_cast<std::uint64_t>(order.price), 8);
    put(order.maker_fee_rate, 1);
    put(order.taker_fee_rate, 1);
    put(order.has_subsidy ? 1 : 0, 1);
    return out;
}

SlotNonce slot_nonce(std::string_view client_id) noexcept {
    const auto digest = sha256({reinterpret_cast<const std::uint8_t*>(client_id.data()), client_id.size()});
    const auto value = read_be(digest);
    // 2^64 is 1 modulo 2^64 - 1, so the limbs just add with an end-around carry
    uint128_t sum = 0;
    for (const auto limb : value) {
        sum += limb;
    }
    while ((sum >> 64) != 0) {
        sum = (sum & k_max_u64) + (sum >> 64);
    }
    auto mod64 = static_cast<std::uint64_t>(sum);
    if (mod64 == k_max_u64) {
        mod64 = 0;
    }
    return SlotNonce{.slot = float_quotient_truncated(mod64, k_max_u32),
                     .nonce = static_cast<std::uint32_t>(mod64 % k_max_u32)};
}

Bytes32 rescue_hash_tx_msg(std::span<const std::uint8_t> message) noexcept {
    constexpr std::size_t k_padded_bits = 736;
    BitPacker bits;
    bits.append(message.size() * 8 <= k_padded_bits ? message : message.first(k_padded_bits / 8));
    bits.pad_to(k_padded_bits);
    std::array<Fe, 4> inputs{};
    const auto count = bits.pack(inputs);

    State state{};
    state[2] = k_fr.from({count, 0, 0, 0});
    for (std::size_t at = 0; at < count; at += 2) {
        state[0] = k_fr.add(state[0], inputs[at]);
        state[1] = k_fr.add(state[1], at + 1 < count ? inputs[at + 1] : k_fr.one());
        rescue_permute(state);
    }
    return le_bytes(k_fr.to(state[0]));
}

Signer::~Signer() {
    OPENSSL_cleanse(secret_.data(), sizeof(secret_));
}

bool Signer::load(std::span<const std::uint8_t> seed) noexcept {
    loaded_ = false;
    if (seed.size() < 32) {
        return false;
    }
    auto effective = sha256(seed);
    for (;;) {
        const auto raw = sha256(effective);
        const auto candidate = read_be(raw);
        if (less(candidate, k_fs.modulus())) {
            secret_ = candidate;
            break;
        }
        effective = raw;
    }
    OPENSSL_cleanse(effective.data(), effective.size());
    const auto pk = affine(base_mul(secret_));
    public_ = pack_point(pk);
    public_x_ = le_bytes(pk.x);
    loaded_ = true;
    return true;
}

bool Signer::load_hex(std::string_view hex) noexcept {
    if (hex.starts_with("0x") || hex.starts_with("0X")) {
        hex.remove_prefix(2);
    }
    std::array<std::uint8_t, 256> bytes{};
    if (hex.size() % 2 != 0 || hex.size() / 2 > bytes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < hex.size() / 2; ++i) {
        const int high = hex_digit(hex[2 * i]);
        const int low = hex_digit(hex[(2 * i) + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    const bool ok = load(std::span{bytes.data(), hex.size() / 2});
    OPENSSL_cleanse(bytes.data(), bytes.size());
    return ok;
}

Signature Signer::sign(std::span<const std::uint8_t> message) const noexcept {
    Signature out{};
    if (!loaded_) {
        return out;
    }
    const auto hashed = rescue_hash_tx_msg(message);
    auto nonce = deterministic_nonce(secret_, hashed);
    const auto r = affine(base_mul(nonce));
    const auto challenge = rescue_h_star(public_x_, le_bytes(r.x), hashed);
    const auto s = k_fs.to(k_fs.add(k_fs.mul(k_fs.from(challenge), k_fs.from(secret_)), k_fs.from(nonce)));
    OPENSSL_cleanse(nonce.data(), sizeof(nonce));
    const auto packed_r = pack_point(r);
    const auto packed_s = le_bytes(s);
    std::memcpy(out.data(), packed_r.data(), 32);
    std::memcpy(out.data() + 32, packed_s.data(), 32);
    return out;
}

} // namespace raikiri::venues::apex::zk
