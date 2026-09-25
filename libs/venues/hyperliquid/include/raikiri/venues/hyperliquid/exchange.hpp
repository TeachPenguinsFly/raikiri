#pragma once

// the action is packed as msgpack for the signature and as json for the body from one call sequence

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/hyperliquid/sign.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: this adapter never loaded it
    UnknownAsset,      // loaded, but with no venue asset index recorded
    BadQuantity,       // does not render as a venue-legal size
    BadPrice,
    // the venue allows five significant figures on a non-integer price regardless of tick
    TooManySignificantDigits,
    BufferTooSmall,
    NotSigned,     // no private key loaded, so nothing can be signed
    NoOrderTarget, // cancel/amend naming neither a venue id nor a client id
    // the venue has no market type: a market command with a price goes out
    // as ioc, one without is refused
    MarketOrderNeedsLimit,
    // alo is a time-in-force here, so post-only ioc cannot be expressed
    PostOnlyConflictsWithTif,
    FokUnsupported,          // the venue offers Alo, Ioc and Gtc, nothing else
    OptionsUnsupported,      // this venue lists none, on any surface
    DatedFuturesUnsupported, // nor any dated futures
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// the venue refuses a nonce at or below the smallest of the 100 highest it has seen per signer
class NonceSource {
public:
    [[nodiscard]] std::uint64_t next(UnixNanos now) noexcept;

private:
    std::uint64_t last_{0};
};

// at most five significant figures unless the price is an integer
[[nodiscard]] bool price_digits_ok(std::string_view text) noexcept;

// requests view scratch owned here and die when the next one is built
class ExchangeBuilder {
public:
    ExchangeBuilder(const Wallet& wallet, bool mainnet) noexcept : wallet_{&wallet}, mainnet_{mainnet} {}

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, std::int64_t asset, std::uint64_t nonce);

    // by oid when present, else cancelByCloid, a separate action with different keys
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, std::int64_t asset, std::uint64_t nonce);

    // native modify; carries the whole replacement order
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    modify_order(const OrderCmd& cmd, const exec::InstrumentTable& table, std::int64_t asset, std::uint64_t nonce);

private:
    [[nodiscard]] static std::uint64_t digits_to_oid(std::string_view text) noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign_and_wrap(std::span<const std::uint8_t> packed, std::string_view action_json, std::uint64_t nonce);

    static constexpr std::size_t k_action_bytes = 1024;

    const Wallet* wallet_;
    bool mainnet_;
    std::array<std::uint8_t, k_action_bytes> packed_{};
    std::array<char, k_action_bytes> action_json_{};
    std::array<char, k_action_bytes + 256> body_{};
};

} // namespace raikiri::venues::hyperliquid
