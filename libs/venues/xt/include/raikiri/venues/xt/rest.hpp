#pragma once

// spot signs the sorted validate-* headers + #METHOD#path[#query][#body]; futures signs appkey, timestamp +
// #path[#params], no method; the header prefix sent must match the signed one; xt's go sdk is the reference, its doc
// example does not reproduce

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// futures host for public data and order entry
[[nodiscard]] std::string_view rest_host() noexcept;

// spot host; spot answers {"rc","mc"} and futures {"returnCode","error"}
[[nodiscard]] std::string_view spot_rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest symbol_list() noexcept;

    [[nodiscard]] net::RestRequest spot_symbol_list() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    // spot signs against its own clock; the hosts drift separately
    [[nodiscard]] net::RestRequest spot_server_time() noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,
    BadPrice,
    // futures size is whole contracts; xt refuses fractions with invalid_quantity_scale
    FractionalContracts,
    BufferTooSmall,
    NotSigned,     // credentials were never loaded
    NoOrderTarget, // cancel/amend with no venue order id
    // xt's clientOrderId is ^[a-zA-Z0-9_]{4,22}$; only a ulid has the 22-character encoding
    ClientIdNotEncodable,
    // a spot market buy needs quoteQty and OrderCmd carries a base size
    MarketBuyNeedsQuoteQty,
    OptionsUnsupported,
    DatedFuturesUnsupported,
    SpotPositionsUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// exhaustive so Option needs a decision; called before the table so an unserved market is not UnknownInstrument
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// a sell that closes a long carries positionSide LONG
[[nodiscard]] std::string_view position_side(Side side, OrderFlags flags) noexcept;

// post-only is the GTX time in force
[[nodiscard]] std::string_view time_in_force(TimeInForce tif, OrderFlags flags) noexcept;

inline constexpr std::uint32_t k_page_size = 100;

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // both trees cancel by venue order id only; a cancel with only our id is refused
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // price and quantity are sent together, so an amend restates both
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // empty symbol means every contract or pair
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id, MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    // the u-based futures wallet only; coin-margined is on dapi.xt.com and spot on sapi
    [[nodiscard]] std::expected<net::RestRequest, BuildError> futures_balances(UnixNanos now);

    // xt pages newest-first and refuses endTime
    [[nodiscard]] std::expected<net::RestRequest, BuildError> bills(UnixNanos since, UnixNanos now);

    // valid 8 hours
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key(UnixNanos now);

    // one call per 10 s; the token lasts 2 days
    [[nodiscard]] std::expected<net::RestRequest, BuildError> ws_token(UnixNanos now);

    // for tests: the bytes that went into the last hmac
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign_spot(
        net::HttpMethod method, std::string_view path, std::string_view query, std::string_view body, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign_futures(
        net::HttpMethod method, std::string_view path, std::string_view query, std::string_view body, UnixNanos now);

    const net::Signer* signer_;

    // reused per build; send a request before building the next
    std::array<char, 1024> body_{};
    std::array<char, 512> query_{};
    std::array<char, 256> path_{};
    std::array<char, 768> target_{};
    std::array<char, 2048> sign_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::xt
