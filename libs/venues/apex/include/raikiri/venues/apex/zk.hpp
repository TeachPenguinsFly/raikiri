#pragma once

// the l2 signature an order carries: rescue over bn254 and schnorr-musig over alt-babyjubjub; every stage is
// pinned to the official sdk's signer in the tests

#include <raikiri/common/int128.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::apex::zk {

inline constexpr std::size_t k_contract_bytes = 36;
inline constexpr std::size_t k_signature_bytes = 64;

using Bytes32 = std::array<std::uint8_t, 32>;
// the packed point r, then s as a little-endian scalar
using Signature = std::array<std::uint8_t, k_signature_bytes>;

// the sdk's ContractBuilder; slot, nonce and pair are cut to their wire widths when packed
struct ContractOrder {
    std::uint32_t account_id{0};
    std::uint8_t sub_account_id{0};
    std::uint64_t slot_id{0};
    std::uint32_t nonce{0};
    std::uint32_t pair_id{0};
    uint128_t size{0};
    uint128_t price{0};
    bool buy{false};
    std::uint8_t taker_fee_rate{0};
    std::uint8_t maker_fee_rate{0};
    bool has_subsidy{false};
};

// a 35-bit mantissa times a 5-bit power of ten, rounded down as the sdk does
[[nodiscard]] std::array<std::uint8_t, 5> pack_amount(uint128_t amount) noexcept;

[[nodiscard]] std::array<std::uint8_t, k_contract_bytes> contract_bytes(const ContractOrder& order) noexcept;

struct SlotNonce {
    std::uint64_t slot{0};
    std::uint32_t nonce{0};
};

// from sha256 of the client id; the slot is a correctly rounded float quotient truncated, as python computes it
[[nodiscard]] SlotNonce slot_nonce(std::string_view client_id) noexcept;

// the 32-byte message the schnorr step signs
[[nodiscard]] Bytes32 rescue_hash_tx_msg(std::span<const std::uint8_t> message) noexcept;

class Signer {
public:
    Signer() = default;
    ~Signer();
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    // the account's zk seeds as raw bytes, at least 32 of them
    [[nodiscard]] bool load(std::span<const std::uint8_t> seed) noexcept;

    // the same seeds as hex, with or without 0x
    [[nodiscard]] bool load_hex(std::string_view hex) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    [[nodiscard]] const Bytes32& public_key() const noexcept { return public_; }

    // deterministic: the nonce is an hmac-sha256 drbg over the key and the hashed message
    [[nodiscard]] Signature sign(std::span<const std::uint8_t> message) const noexcept;

private:
    std::array<std::uint64_t, 4> secret_{};
    Bytes32 public_{};
    Bytes32 public_x_{};
    bool loaded_{false};
};

} // namespace raikiri::venues::apex::zk
