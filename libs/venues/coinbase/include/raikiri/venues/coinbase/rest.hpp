#pragma once

// coinbase exchange api (api.exchange.coinbase.com), not advanced trade; spot only

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/coinbase/sign.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

[[nodiscard]] std::string_view rest_host() noexcept;

// client_oid must be a lowercase uuidv4 or coinbase ignores the order;
// derived from the ulid deterministically but not reversibly
inline constexpr std::size_t k_uuid_len = 36;

[[nodiscard]] std::optional<std::array<char, k_uuid_len>>
client_oid_of(const ExchangeClientOrderId& id) noexcept;

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,
    BadPrice,
    BufferTooSmall, // the request outgrew its scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel with neither a venue id nor a client id
    // coinbase accepts a malformed client_oid and then ignores it
    ClientOrderIdNotAUlid,
    // spot only; each market is refused by name
    PerpNotListed,
    DatedFuturesNotListed,
    OptionsNotListed,
    // no rest order-modify on coinbase exchange
    AmendUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// checked before the table so an unlisted market is not reported as UnknownInstrument
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest products() noexcept;

    [[nodiscard]] net::RestRequest product(std::string_view product_id) noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest book(std::string_view product_id, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

class SignedRequestBuilder {
public:
    // the signer and passphrase live in venue state and outlive the builder
    SignedRequestBuilder(const net::Signer& signer, std::string_view passphrase) noexcept
        : signer_{&signer}, passphrase_{passphrase} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // DELETE /orders/{id}, or /orders/client:{uuid} without a venue id
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // always refuses: no rest modify; the caller emits ModifyRejected
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // `status` is required; omitting it is a 400
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id, MarketType market, UnixNanos now);

    // exposed for tests: the exact bytes that went into the last hmac
    [[nodiscard]] std::string_view last_prehash() const noexcept;

    // GET /orders caps at 1000 per page and defaults to 100
    inline static constexpr std::uint32_t k_max_page = 1000;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view target, std::string_view body, UnixNanos now);

    const net::Signer* signer_;
    std::string_view passphrase_;

    // one request must be sent before the next is built
    std::array<char, 512> target_{};
    std::array<char, 512> body_{};
    std::array<char, 1024> prehash_{};
    std::array<char, 24> timestamp_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::size_t prehash_len_{0};
};

} // namespace raikiri::venues::coinbase
