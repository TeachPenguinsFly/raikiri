#pragma once

// EIP-712 signing for Hyperliquid order entry.
//
// Nothing here resembles the HMAC every CEX adapter in this repo signs with.
// Hyperliquid is an on-chain exchange: an action is msgpack-encoded, hashed
// with keccak-256 alongside a nonce, wrapped in an EIP-712 `Agent` struct, and
// signed with a secp256k1 key into {r, s, v}. The wallet ADDRESS is not the
// credential — the private key is, and it never leaves the Wallet below.
//
// Every stage is pinned to a published vector in the venue's own SDK test
// suite (tests/venues/hyperliquid/venue_test.cpp), because there is no useful
// feedback from the venue if any of them is wrong: a bad signature recovers to
// SOME address, and the answer is "User or API Wallet 0x<a stranger> does not
// exist" — naming an account that changes with every attempt.

#include <raikiri/common/fixed_string.hpp>
#include <raikiri/config/secret.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

inline constexpr std::size_t k_hash_bytes = 32;

using Hash = std::array<std::uint8_t, k_hash_bytes>;

// keccak-256 — ORIGINAL Keccak padding (0x01), NOT NIST SHA3-256 (0x06). The
// two differ in one byte of padding and in nothing else, so SHA3-256 produces a
// perfectly well-formed hash of the wrong thing; OpenSSL ships the latter under
// a name close enough to reach for by mistake. Pinned to the published vector
// keccak256("") = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470.
[[nodiscard]] Hash keccak256(std::span<const std::uint8_t> data) noexcept;

// keccak-256 over msgpack(action) || nonce big-endian || the vault marker.
//
// `vault_address` is "0x" + 40 hex when trading a vault or a sub-account, and
// EMPTY otherwise — the two are not the same preimage: absent writes one 0x00
// byte, present writes 0x01 and then the twenty raw address bytes.
//
// `expires_after` is omitted entirely when zero. When present its marker byte
// is 0x00, NOT the 0x01 that symmetry with the vault marker suggests.
[[nodiscard]] Hash action_hash(std::span<const std::uint8_t> action,
                               std::uint64_t nonce,
                               std::string_view vault_address,
                               std::uint64_t expires_after) noexcept;

// The EIP-712 digest an L1 action is signed over: keccak(0x19 0x01 ||
// domainSeparator || hashStruct(Agent)). The domain is a CONSTANT — chainId
// 1337 and a zero verifying contract, which is not any chain the venue settles
// on — so the separator is computed once and never varies.
//
// `mainnet` selects the Agent's `source`: "a" for mainnet, "b" for testnet. It
// is the only thing stopping a testnet order from replaying on production.
[[nodiscard]] Hash agent_digest(const Hash& action, bool mainnet) noexcept;

// True when two "0x"-prefixed addresses name the same account. Ethereum writes
// an address in MIXED CASE as a checksum, so two spellings of one account
// routinely disagree on capitalisation — and a byte comparison would report a
// correctly configured gateway as misconfigured.
[[nodiscard]] bool same_address(std::string_view a, std::string_view b) noexcept;

// The venue's signature envelope. `r` and `s` are "0x"-prefixed hex with
// leading zeroes STRIPPED, which is what the reference SDK emits and what the
// published vectors show (several are 63 nibbles, not 64).
struct Signature {
    FixedString<66> r{};
    FixedString<66> s{};
    std::uint8_t v{0}; // 27 or 28: the recovery id, offset the way Ethereum does
};

// The secp256k1 signing key, and the only place raw key material is held.
//
// Deterministic by RFC 6979, so a digest signs to exactly one (r, s) — which is
// what makes the published vectors a test rather than a smoke check.
class Wallet {
public:
    Wallet() = default;
    Wallet(const Wallet&) = delete;
    Wallet& operator=(const Wallet&) = delete;

    // 32 bytes of hex, with or without the "0x" every Ethereum tool writes.
    // Refuses a key outside [1, n-1]: zero and n both sign to garbage rather
    // than failing, and n-and-above silently signs as a DIFFERENT key.
    [[nodiscard]] bool load(std::string_view hex_key) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    // The address this key controls, "0x" + 40 lowercase hex. Worth checking
    // against the configured account before the first order: a mismatch is
    // otherwise reported by the venue as an address neither side recognises.
    [[nodiscard]] std::string_view address() const noexcept;

    // Low-s normalised, which the venue requires and which halves the valid
    // signature space: without it roughly one order in two is refused.
    [[nodiscard]] bool sign(const Hash& digest, Signature& out) const noexcept;

private:
    config::Secret key_;
    std::array<char, 42> address_{};
    bool loaded_{false};
};

} // namespace raikiri::venues::hyperliquid
