#pragma once

// Bitfinex v2 REST request building.
//
// TWO HOSTS: api-pub.bitfinex.com serves the public reads and
// api.bitfinex.com every authenticated call. Everything authenticated is a
// POST with a JSON body — there are no GETs and no query strings on that half
// of the API, and an empty payload is the two bytes `{}` rather than nothing.
//
// The signature covers `/api/v2` + path + nonce + THE EXACT BODY BYTES. A
// payload re-serialised between signing and sending is a bad signature, which
// is why the body is written once into the builder's own buffer and both the
// digest and the request point at it.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>
#include <raikiri/venues/bitfinex/wire.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace raikiri::config {
class Secret;
} // namespace raikiri::config

namespace raikiri::venues::bitfinex {

[[nodiscard]] std::string_view rest_host() noexcept;

// api.bitfinex.com — every authenticated call. Nothing public is served here
// and nothing private is served by the other host.
[[nodiscard]] std::string_view private_rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest pair_info(bool futures) noexcept;

    // /v2/platform/status — `[1]` operative, `[0]` maintenance.
    [[nodiscard]] net::RestRequest platform_status() noexcept;

    [[nodiscard]] net::RestRequest tickers() noexcept;

    [[nodiscard]] net::RestRequest book(std::string_view symbol, std::uint32_t len) noexcept;

private:
    std::array<char, 256> target_{};
};

// ---------------------------------------------------------- private ----

enum class BuildError : std::uint8_t {
    UnknownInstrument,
    BadQuantity,
    BadPrice,
    BufferTooSmall,
    NotSigned,
    // Both writes name the order by its VENUE id. Bitfinex will also cancel by
    // `cid`, but only together with `cid_date` — the cid is scoped to the day
    // it was placed — and OrderCmd carries no placement date, so an order
    // known only by our own id has no cancel this adapter can build.
    NoOrderTarget,
    // The OMS mints 26-character ULIDs and Bitfinex's `cid` is a NUMBER. There
    // is no encoding of 128 bits into a JS-safe integer, so the cid is derived
    // rather than carried; an id that is not a ULID has no derivation.
    ClientIdNotDerivable,
    OptionsUnsupported,
    DatedFuturesUnsupported,
    SpotPositionsUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// The markets this adapter serves, named exhaustively so Option is a case the
// compiler forces a decision about rather than something a default arm
// swallows. Called BEFORE the instrument table is read: the table resolves an
// id by its whole value, so an id for a market this adapter never loaded finds
// nothing and would otherwise answer UnknownInstrument — a refusal that blames
// a stale catalogue when the truth is the market is not served here.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// Spot and derivatives take the SAME order through DIFFERENT type names: spot
// is "EXCHANGE LIMIT" and the perpetual plain "LIMIT". The bare form on a spot
// pair opens a MARGIN position instead of buying the asset, which is not a
// refusal — it is a different trade that succeeds.
//
// The time in force is a TYPE here, not a field: IOC and FOK replace LIMIT
// rather than qualifying it. Post-only is the one that stays a flag.
[[nodiscard]] std::string_view order_type(MarketType market, OrderType type, TimeInForce tif) noexcept;

[[nodiscard]] std::int64_t order_flags(OrderFlags flags) noexcept;

// Bitfinex's `cid` is a per-day integer, so a ULID cannot ride in it. The low
// 48 bits of the ULID's own 128 are used — its randomness, not a hash of its
// text — which keeps a retried submit idempotent within the day and leaves the
// venue's duplicate check working. It is NOT invertible: the ULID comes back
// through the venue order id, not through this field.
inline constexpr std::int64_t k_cid_bits = 48;

[[nodiscard]] std::optional<std::int64_t> cid_from(const ExchangeClientOrderId& id) noexcept;

class SignedRequestBuilder {
public:
    SignedRequestBuilder(std::string_view api_key, const config::Secret& secret) noexcept
        : api_key_{api_key}, secret_{&secret} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Native: POST /v2/auth/w/order/update takes a new price and a new amount
    // together, so an amend that moves one still restates the other.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(std::string_view path, std::string_view body, UnixNanos now);

    std::string_view api_key_;
    const config::Secret* secret_;
    NonceSource nonces_;

    std::array<char, 512> body_{};
    std::array<char, 768> sign_msg_{};
    std::array<char, k_hex384_bytes> signature_{};
    std::array<char, 24> nonce_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::bitfinex
