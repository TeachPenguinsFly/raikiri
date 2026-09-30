#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::toobit {

enum class BuildError : std::uint8_t { NoCredentials, BufferTooSmall, UnsupportedOrder, BadValue };

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view
sign_query(const net::Signer& signer, std::string_view query, std::span<char> out) noexcept;

// a request's views point into this builder; send it before building the next.
class RequestBuilder {
public:
    explicit RequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> cancel_order(const OrderCmd& cmd, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> query_order(const ExchangeClientOrderId& id,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    user_trades(std::string_view symbol, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> balance(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_create(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_keepalive(std::string_view key, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_close(std::string_view key, UnixNanos now);

    [[nodiscard]] std::string_view last_signed() const noexcept { return {query_.data(), signed_}; }

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view path, std::size_t query_len);

    const net::Signer* signer_;
    std::array<char, 1024> query_{};
    std::array<char, 1280> target_{};
    std::array<char, net::k_hex_bytes + 1> signature_{};
    std::size_t signed_{0};
};

} // namespace raikiri::venues::toobit
