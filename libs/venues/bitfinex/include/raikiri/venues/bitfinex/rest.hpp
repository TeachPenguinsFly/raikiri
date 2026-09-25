#pragma once

// api-pub serves public reads, api every authenticated post with a json body
// the signature covers the exact body bytes, written once into the builder's buffer

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>

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

[[nodiscard]] std::string_view private_rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest pair_info(bool futures) noexcept;

    // /v2/platform/status: [1] operative, [0] maintenance
    [[nodiscard]] net::RestRequest platform_status() noexcept;

    [[nodiscard]] net::RestRequest tickers() noexcept;

    [[nodiscard]] net::RestRequest book(std::string_view symbol, std::uint32_t len) noexcept;

private:
    std::array<char, 256> target_{};
};

enum class BuildError : std::uint8_t {
    UnknownInstrument,
    BadQuantity,
    BadPrice,
    BufferTooSmall,
    NotSigned,
    // cancel by cid also needs cid_date, which OrderCmd lacks
    NoOrderTarget,
    // cid is a number, so it is derived from the ulid
    ClientIdNotDerivable,
    OptionsUnsupported,
    DatedFuturesUnsupported,
    SpotPositionsUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// exhaustive over markets; checked before the instrument table
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// spot uses "EXCHANGE LIMIT"; plain "LIMIT" on a spot pair opens a margin position
// ioc and fok replace limit in the type; post-only is a flag
[[nodiscard]] std::string_view order_type(MarketType market, OrderType type, TimeInForce tif) noexcept;

[[nodiscard]] std::int64_t order_flags(OrderFlags flags) noexcept;

// cid is the low 48 bits of the ulid's randomness; it does not map back to the ulid
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

    // update takes price and amount together
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    // for tests: the bytes last signed
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
