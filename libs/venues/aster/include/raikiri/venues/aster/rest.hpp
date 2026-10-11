#pragma once

// v3 signs the query string exactly as sent with nonce, user and signer appended, as an eip-712
// Message(string msg) under the AsterSignTransaction domain; the agent wallet signs for the user

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/net/evm.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/aster/endpoints.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::aster {

enum class BuildError : std::uint8_t {
    NoCredentials,
    BufferTooSmall,
    UnsupportedOrderType, // a market order with a price, or similar
    BadValue,             // a price or qty that will not render
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view rest_host(bool testnet, MarketType market = MarketType::Perp) noexcept;

// the eip-712 digest of one v3 query string
[[nodiscard]] net::evm::Hash message_digest(std::string_view msg, std::uint64_t chain_id) noexcept;

// 0x, r, s and v in 65 bytes of hex; empty when the wallet cannot sign
[[nodiscard]] std::string_view sign_message(const net::evm::Wallet& wallet,
                                            std::string_view msg,
                                            std::uint64_t chain_id,
                                            std::span<char> out) noexcept;

class RequestBuilder {
public:
    explicit RequestBuilder(const net::Signer& signer, MarketType market = MarketType::Perp) noexcept
        : signer_{&signer}, market_{market} {}

    // the signer's key slot holds the user's wallet address; the agent wallet signs
    void set_wallet(const net::evm::Wallet& wallet, bool testnet) noexcept {
        wallet_ = &wallet;
        chain_id_ = ep::chain_id(testnet);
    }

    [[nodiscard]] bool has_credentials() const noexcept {
        return wallet_ != nullptr && wallet_->loaded() && !signer_->api_key().empty();
    }

    [[nodiscard]] net::RestRequest instruments() noexcept;
    // the funding interval lives only in fundingInfo
    [[nodiscard]] net::RestRequest funding_info() noexcept;
    [[nodiscard]] net::RestRequest premium_index() noexcept;
    [[nodiscard]] net::RestRequest ticker_24h() noexcept;
    [[nodiscard]] net::RestRequest server_time() noexcept;
    [[nodiscard]] net::RestRequest depth_snapshot(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);
    // limit orders only; both price and quantity are restated
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(std::string_view symbol, const ExchangeClientOrderId& id, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    user_trades(std::string_view symbol, UnixNanos since, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> funding_income(UnixNanos since, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> position_risk(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> account(UnixNanos now);
    // v3 signs the listen-key calls too
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_create(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_keepalive(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_close(UnixNanos now);

    // exposed for tests: the exact bytes the last signed request signed
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    // the parameters are built into `query_`, the auth trio appended and signed, then joined with the path
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view path, std::size_t query_len, UnixNanos now);

    // microseconds, strictly rising: the venue refuses a nonce it has seen
    [[nodiscard]] std::uint64_t next_nonce(UnixNanos now) noexcept;

    std::array<char, 768> query_{};
    std::array<char, 1024> target_{};
    std::array<char, net::evm::k_rsv_hex_bytes> signature_{};
    std::size_t sign_len_{0};
    std::uint64_t last_nonce_{0};
    std::uint64_t chain_id_{ep::chain_id(false)};
    const net::Signer* signer_;
    const net::evm::Wallet* wallet_{nullptr};
    // spot reads the same shapes from its own paths
    MarketType market_{MarketType::Perp};
};

} // namespace raikiri::venues::aster
