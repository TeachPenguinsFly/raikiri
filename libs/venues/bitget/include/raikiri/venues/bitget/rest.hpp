#pragma once

// Bitget v2 REST request building.
//
// Signing rule, and it is the one that quietly breaks adapters: the digest
// covers timestamp + METHOD + requestPath + "?" + queryString + body, base64
// of HMAC-SHA256, and the "?" is the SIGNER's, not the query's. A query that
// carries its own leading "?" doubles it; a GET that signs a body it does not
// send, or a POST that signs a query it does not have, produces a signature
// Bitget answers with 40009 "sign signature error" — which reads like a bad
// secret and costs an afternoon. The tests pin the exact bytes.
//
// Spot and mix are two APIs, not two flavours of one: different paths,
// different payloads, and productType on every mix call. Nothing here shares
// a code path between them beyond the signature.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    UnsupportedMarket, // options; delivery futures until the catalogue lists them
    UnsupportedOrder,  // an order shape Bitget cannot express (see spot market buys)
    BadValue,          // a price or quantity that does not render as a decimal
    BufferTooSmall,    // payload exceeded the scratch buffer (a config bug)
    NotSigned,         // credentials, or the passphrase Bitget signs alongside them
    NothingToSend,     // an empty batch
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view rest_host() noexcept;

// Bitget spells one concept twice: this must agree with ws.hpp's instType.
[[nodiscard]] std::string_view product_type(bool testnet) noexcept;

// The coin every USDT-M contract settles in. Mix place-order demands it and
// the catalogue row does not carry a settle coin, so it is pinned to the
// product rather than read per symbol.
[[nodiscard]] std::string_view margin_coin(std::string_view product) noexcept;

// Bitget caps a cancel batch at 50 and refuses a batch that mixes orderId
// with clientOid, so this adapter cancels by clientOid only.
inline constexpr std::size_t k_max_cancel_batch = 50;

class RequestBuilder {
public:
    // ---- public reads ----
    [[nodiscard]] net::RestRequest contracts(std::string_view product) noexcept;

    // Spot lists its catalogue whole: there is no productType to scope it by.
    [[nodiscard]] net::RestRequest spot_symbols() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol,
                                          std::string_view product) noexcept;

    [[nodiscard]] net::RestRequest current_fund_rate(std::string_view symbol,
                                                     std::string_view product) noexcept;

    [[nodiscard]] net::RestRequest
    orderbook(std::string_view symbol, std::string_view product, std::uint32_t depth) noexcept;

    // ---- signed ----

    // Both views must outlive the builder: the passphrase points into the
    // gateway's locked secret storage and is never copied out of it.
    void set_credentials(const net::Signer& signer, std::string_view passphrase) noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError> create_order(
        const OrderCmd& cmd, const exec::InstrumentTable& table, std::string_view product,
        UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> cancel_order(
        const OrderCmd& cmd, const exec::InstrumentTable& table, std::string_view product,
        UnixNanos now);

    // One symbol per call: mix batch entries carry no symbol of their own, so
    // the caller groups by instrument before calling. Consumes at most
    // k_max_cancel_batch and reports how many it took.
    struct Batch {
        net::RestRequest request;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Batch, BuildError> cancel_batch(
        std::span<const OrderCmd> cmds, const exec::InstrumentTable& table,
        std::string_view product, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view product, UnixNanos now);

    // Neither endpoint needs a symbol when the client id is given, which is
    // why single-order recovery works at all: the caller has an id and no
    // symbol. Mix `order/detail` demands a symbol and is therefore unusable
    // here; the pending list accepts clientOid on its own.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    order_by_client_id(const ExchangeClientOrderId& id, MarketType market,
                       std::string_view product, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    fills(MarketType market, std::string_view product, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    positions(MarketType market, std::string_view product, UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view path, std::string_view body, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view path, std::string_view query, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view path, std::string_view query,
         std::string_view body, UnixNanos now);

    std::array<char, 256> target_{};        // public reads
    std::array<char, 4096> body_{};         // signed POST bodies, and GET queries
    std::array<char, 3072> list_{};         // the batch-cancel array, spliced into body_
    std::array<char, 320> signed_target_{}; // path + "?" + query
    std::array<char, 4608> sign_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
    const net::Signer* signer_{nullptr};
    std::string_view passphrase_{};
};

} // namespace raikiri::venues::bitget
