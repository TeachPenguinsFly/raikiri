#pragma once

// the signed path drops /derivatives; a post signs its form body, a get its query

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
    // kraken spot is another host and auth scheme, not served here
    SpotUnsupported,
    DatedFuturesUnsupported, // FI_/FF_ load as Future; map_instrument drops them
    OptionsUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// checked before the instrument table so an unserved market is named rather than reported unknown
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

    // editorder spells the target orderId; cancelorder spells it order_id
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(UnixNanos now);

    // a cursor: the 100 fills before it, newest first; empty for the newest page
    [[nodiscard]] std::expected<net::RestRequest, BuildError> fills(std::string_view last_fill_time, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(UnixNanos now);

    // for tests: the bytes of the inner sha-256 and the resulting authent
    [[nodiscard]] std::string_view last_sign_message() const noexcept;
    [[nodiscard]] std::string_view last_authent() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view endpoint, std::string_view post_data, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view endpoint, std::string_view query, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view endpoint, std::string_view post_data, UnixNanos now);

    // nonces must increase per key, including two orders in one millisecond
    [[nodiscard]] std::string_view next_nonce(UnixNanos now) noexcept;

    net::Signer* signer_;
    std::uint64_t last_nonce_{0};

    // reused per build; send each request before building the next
    std::array<char, 1024> params_{};
    std::array<char, 1024> target_{};
    std::array<char, 1152> sign_msg_{};
    std::array<char, 24> nonce_{};
    std::array<char, k_authent_bytes> authent_{};
    std::size_t sign_msg_len_{0};
    std::size_t authent_len_{0};
};

} // namespace raikiri::venues::kraken
