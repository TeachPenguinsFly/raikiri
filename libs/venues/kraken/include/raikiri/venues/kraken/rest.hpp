#pragma once

// Kraken Futures derivatives v3 REST request building.
//
// There is no clock endpoint: every response carries `serverTime`, and
// `ticker` is the cheapest such read (~600 bytes against ~1 MiB).
//
// Signing (see sign.hpp for the digest chain) has two traps of its own:
//
//   * the signed path DROPS the "/derivatives" prefix. The request goes to
//     /derivatives/api/v3/sendorder and the signature covers /api/v3/sendorder;
//   * `postData` is the parameter text exactly as it travels. A POST here is
//     form-urlencoded with an EMPTY query, a GET carries its parameters in the
//     query with an empty body, and signing the other one authenticates as a
//     bad key rather than as a malformed request.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/kraken/sign.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace raikiri::venues::kraken {

[[nodiscard]] std::string_view rest_host() noexcept;

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: this adapter never loaded it
    BadQuantity,       // not a positive whole number of the instrument's lots
    BadPrice,
    BufferTooSmall, // payload exceeded the scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel/amend naming neither a venue id nor a client id
    // Kraken Spot is a DIFFERENT PRODUCT: api.kraken.com, its own symbology and
    // an auth scheme that shares only the base64 secret. Nothing in this
    // adapter speaks it, and an order routed here would be signed for a host it
    // never reaches.
    SpotUnsupported,
    DatedFuturesUnsupported, // FI_/FF_ load as Future; map_instrument drops them
    OptionsUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// Named before the instrument table is read by every builder that takes an
// OrderCmd. InstrumentTable::find compares the WHOLE id, so an id for a market
// this adapter never loaded resolves to nothing and would otherwise answer
// UnknownInstrument — a refusal that blames a stale catalogue when the truth is
// that the market is not served here.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest instruments() noexcept;

    // /derivatives/api/v3/tickers/{symbol} — one row plus `serverTime`.
    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest orderbook(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Kraken DOES modify a live order, and spells the target `orderId` here
    // while cancelorder spells the same field `order_id`. The response objects
    // disagree the same way (wire.hpp).
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(UnixNanos now);

    // `last_fill_time` is a CURSOR, not a floor: the endpoint answers the 100
    // fills BEFORE it, newest first. Empty asks for the newest page; paging
    // further back means echoing the oldest `fillTime` this page carried.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> fills(std::string_view last_fill_time,
                                                                    UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(UnixNanos now);

    // Exposed for tests: the exact bytes that went into the inner SHA-256, and
    // the Authent header they produced.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;
    [[nodiscard]] std::string_view last_authent() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view endpoint, std::string_view post_data, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view endpoint, std::string_view query, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> finish(net::HttpMethod method,
                                                                     std::string_view endpoint,
                                                                     std::string_view post_data,
                                                                     UnixNanos now);

    // Kraken rejects a nonce at or below the last one this key used, and two
    // orders fired inside one millisecond would otherwise collide.
    [[nodiscard]] std::string_view next_nonce(UnixNanos now) noexcept;

    net::Signer* signer_;
    std::uint64_t last_nonce_{0};

    // Scratch, reused per build: one request must be sent before the next is
    // built, which is the contract every builder in this repo carries. The
    // RestRequest hands out views into these.
    std::array<char, 1024> params_{};
    std::array<char, 1024> target_{};
    std::array<char, 1152> sign_msg_{};
    std::array<char, 24> nonce_{};
    std::array<char, k_authent_bytes> authent_{};
    std::size_t sign_msg_len_{0};
    std::size_t authent_len_{0};
};

} // namespace raikiri::venues::kraken
