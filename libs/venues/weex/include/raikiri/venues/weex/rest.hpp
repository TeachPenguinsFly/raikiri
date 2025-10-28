#pragma once

// WEEX contract REST request building, public and signed.
//
// THE SIGNATURE IS THE WHOLE RISK HERE, and no credential on this machine has
// ever put one in front of the venue. What is implemented is what WEEX
// documents, transcribed exactly:
//
//   prehash = timestamp + METHOD + requestPath + "?" + queryString + body
//
// with three traps folded into one function rather than repeated at nine call
// sites. The "?" belongs to the SIGNER and appears only when a query exists —
// a GET that signs a bare path while sending "?symbol=..." and a POST that
// signs a query it does not send produce signatures that differ from the
// venue's by bytes nobody can see, and WEEX answers both with the same generic
// authentication refusal a revoked key draws. The method is UPPERCASED, and
// the digest is BASE64 rather than the hex most of this fleet uses.
//
// The timestamp is milliseconds and WEEX refuses one more than 30 seconds from
// its own clock, which is why the signed client reads /capi/v3/market/time at
// start: a laptop that has drifted wears the face of a bad key.
//
// Two public endpoints WEEX does offer are deliberately absent, because a
// builder for a request no caller makes is a facility nobody exercises and
// everybody trusts: /capi/v3/market/depth, which no book needs because the
// depth channel opens with its own snapshot, and /capi/v3/market/ticker/24hr,
// which the ticker channel already pushes and which costs forty weight against
// an allowance of five hundred.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

[[nodiscard]] std::string_view rest_host() noexcept;

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    UnsupportedMarket, // this adapter reaches the contract tree and nothing else
    BadValue,          // a price or quantity the venue could not be given
    NoOrderTarget,     // cancel with neither a venue id nor a client id
    BufferTooSmall,    // the request outgrew its scratch buffer (a config bug)
    NotSigned,         // key, secret or passphrase was never loaded
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// The 36-character ceiling WEEX states on `newClientOrderId`, alongside the
// pattern it states with it: ^[\.A-Z\:/a-z0-9_-]{1,36}$. A 26-character
// Crockford ULID is digits and upper-case letters only, so it fits the pattern
// and the length with room to spare and travels unchanged — there is no
// re-encoding on this venue and no id that means something else on the way
// back.
inline constexpr std::size_t k_max_client_order_id = 36;

// WEEX's own ceiling on one batch cancel. Exceeding it is not an error the
// venue reports — it is ten cancels and a silence about the rest.
inline constexpr std::size_t k_max_cancel_batch = 10;

// Every request this adapter can make. ONE BUILDER IS NOT REENTRANT: each
// method writes into the scratch below and hands out views over it, so a
// request must be sent before the next one is built. The market-data poller and
// the signed path therefore hold separate builders rather than sharing one.
class RequestBuilder {
public:
    RequestBuilder() = default;

    // `passphrase` is a third credential WEEX signs with, and it is as
    // load-bearing as the secret: without it the venue answers -1049, which is
    // not the code a missing key produces.
    RequestBuilder(const net::Signer& signer, std::string_view passphrase) noexcept
        : signer_{&signer}, passphrase_{passphrase} {}

    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    // --- public, unsigned -------------------------------------------------
    [[nodiscard]] net::RestRequest instruments() noexcept;
    // Asked once beside instruments(): the set an order may name.
    [[nodiscard]] net::RestRequest api_trading_symbols() noexcept;
    [[nodiscard]] net::RestRequest premium_index() noexcept;
    [[nodiscard]] net::RestRequest server_time() noexcept;

    // --- signed -----------------------------------------------------------
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> cancel_order(const OrderCmd& cmd,
                                                                           UnixNanos now);

    // Up to `k_max_cancel_batch` in one request. The caller chunks; this
    // refuses anything longer rather than sending a batch WEEX would take the
    // first ten of and say nothing about the rest.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    fills(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> balances(UnixNanos now);

    // The venue's own record of what it charged. A POST with a body, alone
    // among the reads here.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_charges(MarketType market, UnixNanos since, UnixNanos now);

    // Exposed for tests: the exact bytes that went into the HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign(net::HttpMethod method,
                                                                   std::string_view path,
                                                                   std::string_view query,
                                                                   std::string_view body,
                                                                   UnixNanos now);

    const net::Signer* signer_{nullptr};
    std::string_view passphrase_;

    // Scratch, reused per build.
    std::array<char, 1024> body_{};
    std::array<char, 512> list_{};
    std::array<char, 256> query_{};
    std::array<char, 256> target_{};
    std::array<char, 1536> sign_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::weex
