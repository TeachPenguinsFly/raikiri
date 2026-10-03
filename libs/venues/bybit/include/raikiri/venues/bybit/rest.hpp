#pragma once

// signed v5 requests: post signs the body, get signs the query

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::bybit {

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never subscribed to it
    BadQuantity,       // quantity does not render as a venue-legal decimal
    BadPrice,
    BufferTooSmall, // payload exceeded the scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// perps are "linear", spot "spot"; the wrong category returns an empty result
[[nodiscard]] std::string_view category_of(MarketType market) noexcept;

class RequestBuilder {
public:
    RequestBuilder(const net::Signer& signer, std::uint32_t recv_window_ms = 5'000) noexcept
        : signer_{&signer}, recv_window_ms_{recv_window_ms} {}

    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // cancel-all for gateway shutdown
    [[nodiscard]] std::expected<net::RestRequest, BuildError> cancel_all(MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    executions(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    // pair with VenueCaps::funding_charge
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_charges(MarketType market, UnixNanos since, UnixNanos now);

    // no category: one unified wallet backs both markets
    [[nodiscard]] std::expected<net::RestRequest, BuildError> wallet_balance(UnixNanos now);

    [[nodiscard]] net::RestRequest
    instruments(MarketType market, std::string_view cursor = {}, std::string_view base_coin = {});

    // exposed for tests: the exact bytes that go into the hmac
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view target, std::string_view body, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view path, std::string_view query, UnixNanos now);

    const net::Signer* signer_;
    std::uint32_t recv_window_ms_;

    std::array<char, 1024> body_{};
    std::array<char, 256> target_{};
    std::array<char, 512> sign_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::array<char, 16> recv_window_text_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::bybit
