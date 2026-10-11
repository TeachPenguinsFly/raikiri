#pragma once

// v2 order entry: an eip-712 LimitOrderParams signed by the trading key, every private call
// hmac-signed; every stage is pinned to edgex-python-sdk 2.0.1 vectors in the tests

#include <raikiri/common/int128.hpp>
#include <raikiri/config/secret.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/net/evm.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/edgex/wire.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace raikiri::venues::edgex {

namespace evm = raikiri::net::evm;

// a non-negative decimal as python's Decimal holds it, so products keep its trailing zeros
struct Decimal {
    uint128_t mantissa{0};
    std::uint8_t scale{0};
};

[[nodiscard]] std::expected<Decimal, WireError> parse_decimal(std::string_view text) noexcept;

// str(Decimal) for the plain range; "0.100" stays "0.100"
[[nodiscard]] std::string_view format_decimal(Decimal value, std::span<char> out) noexcept;

// what an order signature needs from getMetaData, per contract
struct SigningContract {
    std::uint32_t contract_id{0};
    std::uint64_t resolution{0};
    std::uint32_t collateral_id{0};
    std::uint64_t collateral_resolution{0};
    // max(defaultTakerFeeRate, defaultMakerFeeRate), 0.001 where a rate is blank
    Decimal fee_rate{};
};

struct SigningMeta {
    std::uint64_t chain_id{0}; // nativeChainId, else chainId
    std::array<std::uint8_t, 20> verifying_contract{};
    std::unordered_map<std::uint32_t, SigningContract> contracts;
};

[[nodiscard]] std::expected<SigningMeta, WireError> parse_signing_meta(std::string_view body);

// the first 32 bits of sha256(clientOrderId)
[[nodiscard]] std::uint32_t l2_nonce(std::string_view client_order_id) noexcept;

struct OrderTerms {
    std::uint64_t account_id{0};
    std::uint32_t contract_id{0};
    Side side{Side::Buy};
    std::string_view price{}; // decimal text as sent; the signed value is price * size
    std::string_view size{};
    std::string_view time_in_force{}; // GOOD_TIL_CANCEL, IMMEDIATE_OR_CANCEL, FILL_OR_KILL, POST_ONLY
    std::string_view client_order_id{};
    bool reduce_only{false};
    std::uint64_t now_ms{0};
    // `expireTime`; zero takes the sdk's default of now + 22 days
    std::uint64_t expire_ms{0};
};

// the sdk truncates every amount and rounds only the fee limit, up, to whole collateral units
struct L2Order {
    std::uint32_t nonce{0};
    std::uint64_t expire_ms{0};    // `expireTime`: l2 expiry less 8 days; signed in seconds
    std::uint64_t l2_expire_ms{0}; // now + 30 days
    Decimal value{};
    std::uint64_t limit_fee{0};
    uint128_t amount_synthetic{0};
    uint128_t amount_collateral{0};
    uint128_t amount_fee{0};
};

enum class BuildError : std::uint8_t {
    NotSigned,
    UnknownContract,
    BadDecimal,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::expected<L2Order, BuildError> l2_order(const OrderTerms& terms, const SigningContract& contract);

// name "EdgeX", version "1"
[[nodiscard]] evm::Hash domain_separator(const SigningMeta& meta) noexcept;

[[nodiscard]] evm::Hash order_digest(const evm::Hash& domain,
                                     const OrderTerms& terms,
                                     const SigningContract& contract,
                                     const L2Order& order,
                                     std::span<const std::uint8_t, 20> signer) noexcept;

inline constexpr std::string_view k_create_order = "/api/v2/private/order/createOrder";
inline constexpr std::string_view k_cancel_by_client_id = "/api/v2/private/order/cancelOrderByClientOrderId";
inline constexpr std::string_view k_order_by_client_id = "/api/v2/private/order/getOrderByClientOrderId";
inline constexpr std::string_view k_active_orders = "/api/v2/private/order/getActiveOrderPage";
inline constexpr std::string_view k_fill_page = "/api/v2/private/order/getHistoryOrderFillTransactionPage";
inline constexpr std::string_view k_account_asset = "/api/v2/private/account/getAccountAsset";
// still v1 on v2 accounts
inline constexpr std::string_view k_private_ws = "/api/v1/private/ws";

using Param = std::pair<std::string_view, std::string_view>;

// one request at a time: a returned request views the builder's buffers until the next build
class RequestBuilder {
public:
    RequestBuilder() = default;
    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    // the hmac key is the ascii of base64(secret), as the sdk keys it
    [[nodiscard]] bool load(std::uint64_t account_id,
                            std::string_view api_key,
                            std::string_view passphrase,
                            std::string_view api_secret,
                            std::string_view trading_key) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    [[nodiscard]] std::uint64_t account_id() const noexcept { return account_id_; }

    [[nodiscard]] std::string_view signer() const noexcept { return wallet_.address(); }

    void set_meta(SigningMeta meta);

    [[nodiscard]] const SigningMeta& meta() const noexcept { return meta_; }

    // the body carries every field the sdk sends, nulls included, so its hmac matches the sdk's
    [[nodiscard]] std::expected<net::RestRequest, BuildError> create_order(const OrderTerms& terms);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> cancel_order(std::string_view client_order_id,
                                                                           std::uint64_t now_ms);

    // params sorted by key, as signed; values are sent unencoded
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    get(std::string_view path, std::span<const Param> params, std::uint64_t now_ms);

    // the handshake headers of the private socket; the target names the same timestamp
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> stream_headers(std::uint64_t now_ms);

    [[nodiscard]] std::string stream_target() const;

    // the last create's l2Signature, 130 hex digits without 0x
    [[nodiscard]] std::string_view last_signature() const noexcept { return {signature_.data(), signature_.size()}; }

    // the message the last request signed, for the tests
    [[nodiscard]] std::string_view last_signed_message() const noexcept { return message_; }

private:
    [[nodiscard]] bool
    sign(std::string_view method, std::string_view path, std::string_view payload, std::uint64_t now_ms);

    [[nodiscard]] net::RestRequest request(net::HttpMethod method, std::string_view target, std::string_view body);

    std::uint64_t account_id_{0};
    std::array<char, 24> account_text_{};
    std::size_t account_len_{0};
    std::string api_key_;
    std::string passphrase_;
    config::Secret hmac_key_;
    evm::Wallet wallet_;
    std::array<std::uint8_t, 20> signer_bytes_{};
    SigningMeta meta_;
    evm::Hash domain_{};
    bool loaded_{false};

    std::array<char, 130> signature_{};
    std::array<char, 64> hmac_hex_{};
    std::array<char, 24> timestamp_{};
    std::size_t timestamp_len_{0};
    std::string body_;
    std::string target_;
    std::string message_;
};

// the verdict envelope; `order_id` is data.orderId on a create, the first cancelResultMap value on a cancel
struct WireReply {
    std::string_view code; // "SUCCESS" or the refusal's name
    std::string_view msg;
    std::string_view order_id;
    std::string_view cancel_result;
};

[[nodiscard]] std::expected<WireReply, WireError> parse_reply(std::string_view body);

[[nodiscard]] ErrorClass classify(int http_status, std::string_view code) noexcept;

// the order no longer exists, so a cancel has nothing left to do
[[nodiscard]] bool is_benign_cancel_failure(std::string_view code) noexcept;

struct WireOrder {
    std::string_view id{};
    std::string_view client_order_id{};
    std::uint32_t contract_id{0};
    std::string_view side{};
    std::string_view price{};
    std::string_view size{};
    std::string_view status{}; // PENDING, OPEN, FILLED, CANCELING, CANCELED, UNTRIGGERED
    std::string_view cum_match_size{};
    std::string_view cancel_reason{};
    std::uint64_t updated_ms{0};
};

// a fill row; contract, side and client id are blank where the push leaves them to the order row
struct WireFill {
    std::string_view id;
    std::string_view order_id;
    std::string_view client_order_id;
    std::uint32_t contract_id{0};
    std::string_view side;
    std::string_view size;
    std::string_view price;
    std::string_view fee;
    std::string_view direction; // MAKER or TAKER
    std::uint64_t match_ms{0};
};

struct WirePosition {
    std::uint32_t contract_id{0};
    std::string_view open_size; // signed
    std::string_view open_value;
};

struct WireCollateral {
    std::uint32_t coin_id{0};
    std::string_view amount;
};

inline constexpr std::size_t k_max_rows = 256;

struct TradeBuffer {
    std::array<WireOrder, k_max_rows> orders{};
    std::array<WireFill, k_max_rows> fills{};
    std::array<WirePosition, k_max_rows> positions{};
    std::array<WireCollateral, k_max_rows> collateral{};
};

struct TradeRows {
    std::span<const WireOrder> orders{};
    std::span<const WireFill> fills{};
    std::span<const WirePosition> positions{};
    std::span<const WireCollateral> collateral{};
};

enum class PrivateKind : std::uint8_t {
    Connected,
    Ping,
    Trade, // trade-event: Snapshot, ORDER_UPDATE, FUNDING_SETTLEMENT and the asset updates
    Error,
    Other,
};

struct PrivateFrame {
    PrivateKind kind{PrivateKind::Other};
    std::string_view event{}; // content.event on a trade-event
};

[[nodiscard]] std::expected<PrivateFrame, WireError> parse_private_kind(std::string_view frame);

// the content.data groups of a trade-event
[[nodiscard]] std::expected<TradeRows, WireError> parse_trade_event(std::string_view frame, TradeBuffer& storage);

// data as an array (getOrderByClientOrderId) or as data.dataList (the pages); order rows unless `fills`
[[nodiscard]] std::expected<TradeRows, WireError>
parse_rest_rows(std::string_view body, TradeBuffer& storage, bool fills);

// data.positionList and data.collateralList of getAccountAsset
[[nodiscard]] std::expected<TradeRows, WireError> parse_account_asset(std::string_view body, TradeBuffer& storage);

// OPEN with nothing matched is Accepted, CANCELED is Canceled; the rest leave fills to tell
[[nodiscard]] std::optional<OrderEvent> map_order(const WireOrder& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_order_report(const WireOrder& wire,
                                                                      const InstrumentBase& base);

// `leaves` is the order's remainder after this fill, which only the order row knows
[[nodiscard]] std::optional<OrderEvent>
map_fill(const WireFill& wire, const InstrumentBase& base, QtyLots leaves, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire, const InstrumentBase& base);

} // namespace raikiri::venues::edgex
