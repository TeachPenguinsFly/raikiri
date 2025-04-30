#pragma once

// Signed bybit v5 REST requests.
//
// so a POST that signs the query string, or a GET that signs a body it does
// not send, produces a signature that is wrong in a way the venue reports as
// a generic authentication error — which reads like a bad key and costs
// hours. The two paths are therefore separate functions here, and the tests
// pin the exact bytes that go into the digest.

#include <raikiri/domain/order_cmd.hpp>
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

// bybit's product category. Perps are "linear"; spot is "spot". Every
// endpoint takes it, and sending the wrong one returns an empty result
// rather than an error, which is its own kind of debugging afternoon.
[[nodiscard]] std::string_view category_of(MarketType market) noexcept;

class RequestBuilder {
public:
    RequestBuilder(const net::Signer& signer, std::uint32_t recv_window_ms = 5'000) noexcept
        : signer_{&signer}, recv_window_ms_{recv_window_ms} {}

    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    // Order entry. `now` must be venue-aligned time: bybit rejects anything
    // outside recvWindow of ITS clock, so the caller feeds the clock-sync
    // corrected value rather than the local one.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // The dead-man switch every venue gateway needs on its way down.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> cancel_all(MarketType market,
                                                                         UnixNanos now);

    // Reconciliation pulls.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    executions(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // Public: the instrument catalogue needs no credentials, so it is built
    // unsigned — sending a signature here would only risk leaking a stale one
    // to an endpoint that never asked for it.
    [[nodiscard]] net::RestRequest instruments(MarketType market);

    // Exposed for tests: the exact bytes that go into the HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view target, std::string_view body, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view path, std::string_view query, UnixNanos now);

    const net::Signer* signer_;
    std::uint32_t recv_window_ms_;

    // Scratch, reused per build. Sized for the largest payload any of these
    // endpoints produces, with room to spare — overflow is reported rather
    // than truncated, because a truncated signed body authenticates as
    // something we did not mean to send.
    std::array<char, 1024> body_{};
    std::array<char, 256> target_{};
    std::array<char, 512> sign_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::array<char, 16> recv_window_text_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::bybit
