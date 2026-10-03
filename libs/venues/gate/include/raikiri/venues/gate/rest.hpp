#pragma once

// hmac-sha512, hex, over method, /api/v4 path, query, sha512(body) and a timestamp in seconds;
// the query is signed url-decoded

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

// gate keys the futures api on settle currency; widening it needs map_instrument to handle inverse contracts
inline constexpr std::string_view k_settle_usdt = "usdt";

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contracts(std::string_view settle) noexcept;
    [[nodiscard]] net::RestRequest tickers(std::string_view settle) noexcept;

    // GET /spot/currency_pairs
    [[nodiscard]] net::RestRequest currency_pairs() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest
    order_book(std::string_view settle, std::string_view contract, std::uint32_t depth) noexcept;

private:
    std::array<char, 256> target_{};
};

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,       // quantity does not render as a venue-legal size
    BadPrice,
    BufferTooSmall, // payload exceeded the scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel/amend with neither a venue id nor a client id
    // gate spot prices a market buy in quote currency, which base lots cannot express
    SpotMarketBuyNotExpressible,
    OptionsUnsupported,      // Gate lists options; this adapter does not trade them
    DatedFuturesUnsupported, // Gate's /delivery API is a separate surface
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// gate requires the t- prefix and allows 28 bytes after it; k_client_id_bytes includes the prefix
inline constexpr std::string_view k_client_id_prefix = "t-";
inline constexpr std::size_t k_client_id_body_max = 28;
inline constexpr std::size_t k_client_id_bytes = k_client_id_prefix.size() + k_client_id_body_max;

static_assert(sizeof(ExchangeClientOrderId::data) <= k_client_id_body_max,
              "gate rejects a client order id longer than 28 bytes after the t- prefix");

// renders t-<ulid> into caller storage; empty when it does not fit
[[nodiscard]] std::string_view client_id_text(const ExchangeClientOrderId& id, std::span<char> out) noexcept;

// the venue id when known, else t-<ulid>; cancel and amend name the order by it
[[nodiscard]] std::string_view order_target(const OrderCmd& cmd, std::span<char> out) noexcept;

// text without the t- prefix is one of gate's own sources and is not ours
[[nodiscard]] std::string_view client_id_from_text(std::string_view text) noexcept;

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // futures takes a bare id array; spot pairs each id with its currency_pair; both cap at 20
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market, UnixNanos now);

    // perp only: spot's single-order read needs the currency_pair
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id, MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    // funding settlements from the account book; pairs with VenueCaps::funding_charge
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_book(MarketType market, UnixNanos since, UnixNanos now);

    // its user field is the id the futures private subscribe needs first
    [[nodiscard]] std::expected<net::RestRequest, BuildError> futures_account(UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

    inline static constexpr std::size_t k_max_batch_cancel = 20;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view path, std::string_view query, std::string_view body, UnixNanos now);

    const net::Signer* signer_;

    // reused per build: send one request before building the next
    std::array<char, 2048> body_{};
    std::array<char, 256> path_{};
    std::array<char, 256> query_{};
    std::array<char, 320> target_{};
    std::array<char, 640> sign_msg_{};
    std::array<char, net::k_hex512_bytes> body_hash_{};
    std::array<char, net::k_hex512_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::gate
