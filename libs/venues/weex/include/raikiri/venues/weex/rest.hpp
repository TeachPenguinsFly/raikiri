#pragma once

// prehash = timestamp + METHOD + requestPath + "?" + queryString + body, with "?" only when a query exists
// method uppercased, digest base64, timestamp in ms within 30 s of the venue clock

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

// weex's newClientOrderId limit; the 26-char ulid fits its pattern unchanged
inline constexpr std::size_t k_max_client_order_id = 36;

// weex's limit per batch cancel; it ignores the rest silently
inline constexpr std::size_t k_max_cancel_batch = 10;

// not reentrant: send each request before building the next, so callers hold separate builders
class RequestBuilder {
public:
    RequestBuilder() = default;

    // without the passphrase weex answers -1049
    RequestBuilder(const net::Signer& signer, std::string_view passphrase) noexcept
        : signer_{&signer}, passphrase_{passphrase} {}

    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    [[nodiscard]] net::RestRequest instruments() noexcept;
    // the set an order may name
    [[nodiscard]] net::RestRequest api_trading_symbols() noexcept;
    [[nodiscard]] net::RestRequest premium_index() noexcept;
    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> cancel_order(const OrderCmd& cmd,
                                                                           UnixNanos now);

    // at most k_max_cancel_batch; the caller chunks
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    fills(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> balances(UnixNanos now);

    // a POST with a body
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_charges(MarketType market, UnixNanos since, UnixNanos now);

    // exposed for tests: the exact bytes that went into the hmac
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign(net::HttpMethod method,
                                                                   std::string_view path,
                                                                   std::string_view query,
                                                                   std::string_view body,
                                                                   UnixNanos now);

    const net::Signer* signer_{nullptr};
    std::string_view passphrase_;

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
