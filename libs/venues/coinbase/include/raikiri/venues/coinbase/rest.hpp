#pragma once

// Coinbase Exchange REST request building and signing.
//
// This is the EXCHANGE API (api.exchange.coinbase.com), the former Coinbase
// Pro, not the newer Advanced Trade API on api.coinbase.com. They disagree on
// product naming, on field names, and on whether public reads need a key —
// these do not, which is what makes Coinbase usable as a reference venue with
// no credentials on the box. Order entry does need one, and the scheme is in
// sign.hpp beside this.
//
// SPOT ONLY, and that is the entity rather than the adapter: this Coinbase
// legal entity lists no perpetual, no dated future and no option, so every
// other market is refused by name here rather than left to fail as an
// instrument nobody loaded.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/coinbase/sign.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

[[nodiscard]] std::string_view rest_host() noexcept;

// Coinbase's client order id MUST be a lowercase hyphenated UUIDv4. An id in
// any other shape "will be accepted via REST but NOT processed and will not be
// visible in future REST calls" — the venue's own words, and the reason this
// is a conversion rather than a copy: the OMS mints 26-character Crockford
// ULIDs, which are 128 bits exactly like a UUID but spelled nothing like one.
//
// The conversion is deterministic, so the same ULID always names the same
// order and GET /orders/client:<uuid> resolves without any local table. It is
// NOT reversible: forcing the version and variant bits overwrites six of the
// ULID's eighty random bits, which is why the adapter keeps a memo for the
// return trip and says so where a sweep cannot fill one in.
inline constexpr std::size_t k_uuid_len = 36;

[[nodiscard]] std::optional<std::array<char, k_uuid_len>>
client_oid_of(const ExchangeClientOrderId& id) noexcept;

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,
    BadPrice,
    BufferTooSmall, // the request outgrew its scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel with neither a venue id nor a client id
    // The OMS's id was not a 26-character ULID, so no UUID can be derived from
    // it. Sending it raw is the one thing that must not happen: Coinbase
    // accepts a malformed client_oid and then behaves as if it were never sent.
    ClientOrderIdNotAUlid,
    // This entity lists spot only. Each of the three is named separately
    // because "unsupported market" tells an operator nothing about whether to
    // change the config or the adapter.
    PerpNotListed,
    DatedFuturesNotListed,
    OptionsNotListed,
    // Coinbase Exchange has no order-modify on its REST API. Its websocket
    // reports a `change` with reason `modify_order`, which the FIX gateway can
    // produce and this one cannot.
    AmendUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// Named before the instrument table is read by every builder that takes an
// OrderCmd. InstrumentTable::find compares the WHOLE id, so an id for a market
// this adapter never loaded resolves to nothing and would otherwise answer
// UnknownInstrument — a refusal that blames a stale catalogue when the truth is
// that the market is not listed here at all.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest products() noexcept;

    [[nodiscard]] net::RestRequest product(std::string_view product_id) noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest book(std::string_view product_id, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

class SignedRequestBuilder {
public:
    // Both are held by reference for the life of the builder: the signer and
    // the passphrase live in the shared venue state, which is created before
    // any builder and outlives every one of them.
    SignedRequestBuilder(const net::Signer& signer, std::string_view passphrase) noexcept
        : signer_{&signer}, passphrase_{passphrase} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // DELETE /orders/{id}, or DELETE /orders/client:{uuid} when the order
    // store has no venue id yet.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Always refuses. Coinbase Exchange's REST API has no modify, so an amend
    // that looked like it worked would be a lie about the book; the caller
    // turns this into a ModifyRejected and the strategy re-quotes.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // `status` is a REQUIRED query parameter, and omitting it is a 400 rather
    // than a default.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id, MarketType market, UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_prehash() const noexcept;

    // GET /orders caps at 1000 per page and defaults to 100.
    inline static constexpr std::uint32_t k_max_page = 1000;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view target, std::string_view body, UnixNanos now);

    const net::Signer* signer_;
    std::string_view passphrase_;

    // Scratch, reused per build: one request must be sent before the next is
    // built, which is the contract every builder in this repo carries.
    std::array<char, 512> target_{};
    std::array<char, 512> body_{};
    std::array<char, 1024> prehash_{};
    std::array<char, 24> timestamp_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::size_t prehash_len_{0};
};

} // namespace raikiri::venues::coinbase
