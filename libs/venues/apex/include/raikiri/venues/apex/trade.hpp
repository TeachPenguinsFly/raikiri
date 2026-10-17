#pragma once

// v3 order entry: each order carries a zkLink l2 signature, every private call an hmac; every stage is pinned to
// apexomni 3.4.2 vectors in the tests

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/config/secret.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/apex/wire.hpp>
#include <raikiri/venues/apex/zk.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace raikiri::venues::apex {

// what a signature needs from /v3/symbols, per order symbol (BTC-USDT)
struct SigningContract {
    std::string cross_symbol; // BTCUSDT, the instrument's venue symbol
    std::uint32_t l2_pair_id{0};
    // decimals of the settle asset; price and size are both scaled by it
    std::uint8_t decimals{0};
};

struct SigningMeta {
    std::unordered_map<std::string, SigningContract> contracts;
};

[[nodiscard]] std::expected<SigningMeta, WireError> parse_signing_meta(std::string_view body);

// what a signature needs from /v3/account
struct AccountTerms {
    std::uint64_t account_id{0};
    FixedE9 taker_fee_rate{};
    FixedE9 maker_fee_rate{};
};

struct OrderTerms {
    std::string_view symbol{}; // BTC-USDT
    Side side{Side::Buy};
    FixedE9 price{};
    FixedE9 size{};
    std::string_view time_in_force{}; // GOOD_TIL_CANCEL, POST_ONLY, IMMEDIATE_OR_CANCEL, FILL_OR_KILL
    std::string_view client_id{};
    bool reduce_only{false};
    // `timestampSeconds`; the order expires 28 days after it
    std::uint64_t now_s{0};
};

enum class BuildError : std::uint8_t {
    NotSigned,
    NoAccount,
    UnknownSymbol,
    BadAmount,
    BadFeeRate,
    BadText,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

struct L2Order {
    zk::ContractOrder contract{};
    std::uint64_t expiration_ms{0};
    // price * size * taker rate, rounded up to six places
    std::uint64_t limit_fee_e6{0};
};

[[nodiscard]] std::expected<L2Order, BuildError>
l2_order(const OrderTerms& terms, const SigningContract& contract, const AccountTerms& account);

inline constexpr std::string_view k_create_order = "/api/v3/order";
inline constexpr std::string_view k_cancel_by_id = "/api/v3/delete-order";
inline constexpr std::string_view k_cancel_by_client_id = "/api/v3/delete-client-order-id";
inline constexpr std::string_view k_open_orders = "/api/v3/open-orders";
inline constexpr std::string_view k_order_by_client_id = "/api/v3/order-by-client-order-id";
inline constexpr std::string_view k_fills = "/api/v3/fills";
inline constexpr std::string_view k_account = "/api/v3/account";
inline constexpr std::string_view k_ws_private = "/realtime_private";
inline constexpr std::string_view k_ws_topic = "ws_zk_accounts_v3";

using Param = std::pair<std::string_view, std::string_view>;

// one request at a time: a returned request views the builder's buffers until the next build
class RequestBuilder {
public:
    RequestBuilder() = default;
    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    // the hmac key is the ascii of base64(secret), as the sdk keys it; seeds are hex
    [[nodiscard]] bool load(std::string_view api_key,
                            std::string_view passphrase,
                            std::string_view api_secret,
                            std::string_view zk_seeds) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    void set_meta(SigningMeta meta);

    [[nodiscard]] const SigningMeta& meta() const noexcept { return meta_; }

    void set_account(const AccountTerms& account) noexcept;

    [[nodiscard]] bool has_account() const noexcept { return account_.account_id != 0; }

    [[nodiscard]] const zk::Signer& signer() const noexcept { return signer_; }

    // every field the sdk sends, its "False" and "None" spellings included, so the hmac matches the sdk's
    [[nodiscard]] std::expected<net::RestRequest, BuildError> create_order(const OrderTerms& terms,
                                                                           std::uint64_t now_ms);

    // `path` is k_cancel_by_id or k_cancel_by_client_id
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel(std::string_view path, std::string_view id, std::uint64_t now_ms);

    // params are signed and sent in the order given
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    get(std::string_view path, std::span<const Param> params, std::uint64_t now_ms);

    // the private socket's target, with the timestamp the login frame repeats
    [[nodiscard]] std::string stream_target(std::uint64_t now_ms);

    [[nodiscard]] std::string login_frame();

    // the last create's l2 signature, 0x and 128 hex digits
    [[nodiscard]] std::string_view last_signature() const noexcept { return {signature_.data(), signature_.size()}; }

    [[nodiscard]] std::string_view last_signed_message() const noexcept { return message_; }

    [[nodiscard]] std::string_view last_hmac() const noexcept { return {hmac_.data(), hmac_len_}; }

private:
    [[nodiscard]] bool
    sign(std::string_view method, std::string_view path, std::string_view data, std::uint64_t now_ms);

    [[nodiscard]] net::RestRequest request(net::HttpMethod method, std::string_view target, std::string_view body);

    std::string api_key_;
    std::string passphrase_;
    config::Secret hmac_key_;
    zk::Signer signer_;
    SigningMeta meta_;
    AccountTerms account_{};
    bool loaded_{false};

    std::array<char, 130> signature_{};
    std::array<char, 64> hmac_{};
    std::size_t hmac_len_{0};
    std::array<char, 24> timestamp_{};
    std::size_t timestamp_len_{0};
    std::string body_;
    std::string target_;
    std::string message_;
};

// data on success; code and msg on a refusal
struct WireReply {
    bool ok{false};
    std::string_view code;
    std::string_view msg;
    std::string_view order_id; // data.id on a create
};

[[nodiscard]] std::expected<WireReply, WireError> parse_reply(std::string_view body);

[[nodiscard]] ErrorClass classify(int http_status, std::string_view code) noexcept;

// the order is already gone, so a cancel has nothing left to do
[[nodiscard]] bool is_benign_cancel_failure(std::string_view msg) noexcept;

struct WireOrder {
    std::string_view id{};
    std::string_view client_order_id{};
    std::string_view symbol{};
    std::string_view side{};
    std::string_view price{};
    std::string_view size{};
    std::string_view status{}; // PENDING, OPEN, FILLED, CANCELED, EXPIRED, UNTRIGGERED
    std::string_view cum_fill_size{};
    std::string_view cancel_reason{};
    std::uint64_t updated_ms{0};
};

struct WireFill {
    std::string_view id{};
    std::string_view order_id{};
    std::string_view client_order_id{};
    std::string_view symbol{};
    std::string_view side{};
    std::string_view size{};
    std::string_view price{};
    std::string_view fee{};
    std::string_view liquidity{}; // MAKER or TAKER; the rest page calls it direction
    std::uint64_t created_ms{0};
};

struct WirePosition {
    std::string_view symbol{};
    std::string_view side{}; // LONG or SHORT
    std::string_view size{};
    std::string_view entry_price{};
};

// a contract wallet's balance leaves open pnl out
struct WireWallet {
    std::string_view token{};
    std::string_view balance{};
};

inline constexpr std::size_t k_max_rows = 256;

inline constexpr std::size_t k_max_wallets = 16;

struct PrivateBuffer {
    std::array<WireOrder, k_max_rows> orders{};
    std::array<WireFill, k_max_rows> fills{};
    std::array<WirePosition, k_max_rows> positions{};
    std::array<WireWallet, k_max_wallets> wallets{};
};

struct TradeRows {
    std::span<const WireOrder> orders{};
    std::span<const WireFill> fills{};
    std::span<const WirePosition> positions{};
};

// data.id, the fee rates, data.positions and data.contractWallets
struct AccountRows {
    AccountTerms terms{};
    std::span<const WirePosition> positions{};
    std::span<const WireWallet> wallets{};
};

[[nodiscard]] std::expected<AccountRows, WireError> parse_account(std::string_view body, PrivateBuffer& storage);

// data as one order, as an array, or as a page under orders; fill rows when `fills`
[[nodiscard]] std::expected<TradeRows, WireError>
parse_rest_rows(std::string_view body, PrivateBuffer& storage, bool fills);

enum class PrivateKind : std::uint8_t {
    Ping,
    Ack, // the answer to login or subscribe
    Account,
    Other,
};

struct PrivateFrame {
    PrivateKind kind{PrivateKind::Other};
    bool success{false};      // on an ack
    bool snapshot{false};     // on an account push
    std::string_view value{}; // the ping's argument, or the acked op
};

[[nodiscard]] std::expected<PrivateFrame, WireError> parse_private_kind(std::string_view frame);

// contents.orders, contents.fills and contents.positions of a ws_zk_accounts_v3 push
[[nodiscard]] std::expected<TradeRows, WireError> parse_account_push(std::string_view frame, PrivateBuffer& storage);

[[nodiscard]] std::optional<OrderEvent> map_order(const WireOrder& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_order_report(const WireOrder& wire,
                                                                      const InstrumentBase& base);

// `leaves` is the order's remainder after this fill, which only the order row knows
[[nodiscard]] std::optional<OrderEvent>
map_fill(const WireFill& wire, const InstrumentBase& base, QtyLots leaves, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire, const InstrumentBase& base);

// nullopt for an unreadable row
[[nodiscard]] std::optional<BalanceReport> map_balance(const WireWallet& wire, UnixNanos now);

} // namespace raikiri::venues::apex
