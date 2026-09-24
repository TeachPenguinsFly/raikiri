#pragma once

// signs timestamp, method, path, "?" query and body; the signer adds the "?" itself

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
    UnknownInstrument, // not loaded in the table
    UnsupportedMarket, // options; delivery futures until the catalogue lists them
    UnsupportedOrder,  // an order shape Bitget cannot express (see spot market buys)
    BadValue,          // a price or quantity that does not render as a decimal
    BufferTooSmall,    // payload exceeded the scratch buffer (a config bug)
    NotSigned,         // credentials, or the passphrase Bitget signs alongside them
    NothingToSend,     // an empty batch
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view rest_host() noexcept;

// must agree with ws.hpp's instType
[[nodiscard]] std::string_view product_type(bool testnet) noexcept;

// usdt-m contracts settle in usdt; the catalogue row carries no settle coin
[[nodiscard]] std::string_view margin_coin(std::string_view product) noexcept;

// bitget caps a batch at 50 and refuses mixing orderId with clientOid, so cancels use clientOid
inline constexpr std::size_t k_max_cancel_batch = 50;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contracts(std::string_view product) noexcept;

    [[nodiscard]] net::RestRequest spot_symbols() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol,
                                          std::string_view product) noexcept;

    [[nodiscard]] net::RestRequest current_fund_rate(std::string_view symbol,
                                                     std::string_view product) noexcept;

    [[nodiscard]] net::RestRequest
    orderbook(std::string_view symbol, std::string_view product, std::uint32_t depth) noexcept;

    // both views must outlive the builder; the passphrase is never copied
    void set_credentials(const net::Signer& signer, std::string_view passphrase) noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd,
                 const exec::InstrumentTable& table,
                 std::string_view product,
                 UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd,
                 const exec::InstrumentTable& table,
                 std::string_view product,
                 UnixNanos now);

    // one symbol per call; consumes at most k_max_cancel_batch and reports how many
    struct Batch {
        net::RestRequest request;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Batch, BuildError> cancel_batch(std::span<const OrderCmd> cmds,
                                                                const exec::InstrumentTable& table,
                                                                std::string_view product,
                                                                UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view product, UnixNanos now);

    // by client id with no symbol; mix order/detail needs one, so the pending list is used
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    order_by_client_id(const ExchangeClientOrderId& id,
                       MarketType market,
                       std::string_view product,
                       UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    fills(MarketType market, std::string_view product, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    positions(MarketType market, std::string_view product, UnixNanos now);

    // mix wallet funding rows only; spot is refused
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    settle_bills(MarketType market, std::string_view product, UnixNanos since, UnixNanos now);

    // for tests: the bytes of the last hmac
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view path, std::string_view body, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view path, std::string_view query, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign(net::HttpMethod method,
                                                                   std::string_view path,
                                                                   std::string_view query,
                                                                   std::string_view body,
                                                                   UnixNanos now);

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
