#pragma once

// Hyperliquid /exchange action building.
//
// One path serves every write — there are no per-action targets — and the body
// is always {action, nonce, signature, vaultAddress}. What varies is `action`,
// and `action` is hashed as MSGPACK before it is signed, so this file emits the
// same structure twice: once packed for the signature and once as JSON for the
// wire. Both come from one call sequence, because a body that says something
// the signed bytes did not is a valid signature over an order nobody placed —
// and the venue's only complaint would name an address we do not own.

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
    // Five significant figures is a HARD venue limit on any non-integer price,
    // independent of the tick grid: 1234.56 is refused on an asset whose tick
    // is 0.01. Caught here so the refusal names the rule rather than arriving
    // as "Price must be divisible by tick size."
    TooManySignificantDigits,
    BufferTooSmall,
    NotSigned,     // no private key loaded, so nothing can be signed
    NoOrderTarget, // cancel/amend naming neither a venue id nor a client id
    // Hyperliquid has NO market order type. The reference SDK fakes one with an
    // IOC limit at a slippage-adjusted price it computes from the mid — a mid
    // this adapter is not entitled to invent on the caller's behalf. A Market
    // command carrying a price is sent as that IOC; one without is refused.
    MarketOrderNeedsLimit,
    // ALO is a time-in-force here, not a flag beside one, so "post-only IOC"
    // has no spelling. Silently dropping either half changes the order.
    PostOnlyConflictsWithTif,
    FokUnsupported,          // the venue offers Alo, Ioc and Gtc, nothing else
    OptionsUnsupported,      // this venue lists none, on any surface
    DatedFuturesUnsupported, // nor any dated futures
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// The replay guard, not a timestamp. The venue keeps the 100 highest nonces it
// has seen per signer and refuses anything at or below the smallest of them, so
// two orders inside one millisecond need two nonces — and a clock that steps
// backwards must not be allowed to reissue one.
class NonceSource {
public:
    [[nodiscard]] std::uint64_t next(UnixNanos now) noexcept;

private:
    std::uint64_t last_{0};
};

// True when `text` is a price this venue will accept: at most five significant
// figures unless it is an integer, which is always allowed however long.
[[nodiscard]] bool price_digits_ok(std::string_view text) noexcept;

// One builder per execution client. Holds the scratch the request views point
// into, so a request is dead the moment the next one is built.
class ExchangeBuilder {
public:
    ExchangeBuilder(const Wallet& wallet, bool mainnet) noexcept
        : wallet_{&wallet}, mainnet_{mainnet} {}

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd,
                 const exec::InstrumentTable& table,
                 std::int64_t asset,
                 std::uint64_t nonce);

    // By venue order id when the command carries one, by cloid otherwise. The
    // two are DIFFERENT actions with different key names ("a"/"o" against
    // "asset"/"cloid"), not one action with an alternative field.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, std::int64_t asset, std::uint64_t nonce);

    // Native modify: cancel and replace in one action, so the order never
    // leaves the book. Carries the whole replacement order, not a delta.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    modify_order(const OrderCmd& cmd,
                 const exec::InstrumentTable& table,
                 std::int64_t asset,
                 std::uint64_t nonce);

private:
    [[nodiscard]] static std::uint64_t digits_to_oid(std::string_view text) noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign_and_wrap(
        std::span<const std::uint8_t> packed, std::string_view action_json, std::uint64_t nonce);

    // The longest action is a modify: a full order wire plus a 34-char cloid,
    // twice over. 1024 is slack, not a measurement.
    static constexpr std::size_t k_action_bytes = 1024;

    const Wallet* wallet_;
    bool mainnet_;
    std::array<std::uint8_t, k_action_bytes> packed_{};
    std::array<char, k_action_bytes> action_json_{};
    std::array<char, k_action_bytes + 256> body_{};
};

} // namespace raikiri::venues::hyperliquid
