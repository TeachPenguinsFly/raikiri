#pragma once

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace raikiri::venues::bitunix {

enum class BuildError : std::uint8_t {
    UnknownInstrument,
    UnsupportedMarket,
    BadValue,
    NoOrderTarget,
    NotSigned,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// sha256 hex of (sha256 hex of nonce, timestamp, key, sorted key-value query, body) and the secret
[[nodiscard]] std::string_view sign_request(const net::Signer& signer,
                                            std::string_view nonce,
                                            std::string_view timestamp,
                                            std::string_view query,
                                            std::string_view body,
                                            std::span<char> out) noexcept;

class RequestBuilder {
public:
    explicit RequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    place(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    modify(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> pending_orders(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> history_trades(UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> account(UnixNanos now);

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view path, UnixNanos now);

    void query(std::string_view key, std::string_view value);

    const net::Signer* signer_;
    std::uint64_t nonce_seq_{0};
    std::string body_;
    std::string query_text_;
    std::string query_sign_;
    std::string target_;
    std::array<char, 33> nonce_{};
    std::array<char, 24> timestamp_{};
    std::array<char, net::k_hex_bytes> signature_{};
};

} // namespace raikiri::venues::bitunix
