#pragma once

// binance signs the query string exactly as sent, for every method including post

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::binance {

enum class BuildError : std::uint8_t {
    NoCredentials,
    BufferTooSmall,
    UnsupportedOrderType, // a market order with a price, or similar
    BadValue,             // a price or qty that will not render
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view rest_host(bool testnet, MarketType market = MarketType::Perp) noexcept;

// binance checks the timestamp against its own clock; pass the venue-corrected time as now
inline constexpr std::uint32_t k_recv_window_ms = 5'000;

class RequestBuilder {
public:
    explicit RequestBuilder(const net::Signer& signer, MarketType market = MarketType::Perp) noexcept
        : signer_{&signer}, spot_{market == MarketType::Spot} {}

    // the public reads; orders and the account stay on the futures api

    [[nodiscard]] net::RestRequest instruments() noexcept;
    // the settlement schedule is stated nowhere else
    [[nodiscard]] net::RestRequest funding_info() noexcept;
    [[nodiscard]] net::RestRequest premium_index() noexcept;
    [[nodiscard]] net::RestRequest ticker_24h() noexcept;
    [[nodiscard]] net::RestRequest server_time() noexcept;
    [[nodiscard]] net::RestRequest depth_snapshot(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(std::string_view symbol, const ExchangeClientOrderId& id, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    user_trades(std::string_view symbol, UnixNanos since, UnixNanos now);

    // funding settlements from the income ledger; pairs with VenueCaps::funding_charge
    [[nodiscard]] std::expected<net::RestRequest, BuildError> funding_income(UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> position_risk(UnixNanos now);

    // wallet rows and the margin mode that qualifies them, in one answer
    [[nodiscard]] std::expected<net::RestRequest, BuildError> account(UnixNanos now);

    // listen key: header auth only, unsigned
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_create();
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_keepalive();
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_close();

    // Exposed for tests: the exact bytes the last signed request signed.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view path, std::size_t query_len);

    [[nodiscard]] net::RestRequest header_only(net::HttpMethod method, std::string_view path) noexcept;

    std::array<char, 512> query_{};
    std::array<char, 768> target_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t sign_len_{0};
    const net::Signer* signer_;
    bool spot_{false};
};

} // namespace raikiri::venues::binance
