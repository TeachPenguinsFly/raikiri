#pragma once

// usdt perpetual order entry: signed rest out, the private orders channel back. a request signs
// path + method + timestamp + nonce + body; the hex hmac is then base64'd as text, not as bytes

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/error.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/blofin/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace raikiri::venues::blofin {

inline constexpr std::string_view k_ws_private = "/ws/private";
inline constexpr std::string_view k_place_order = "/api/v1/trade/order";
inline constexpr std::string_view k_cancel_order = "/api/v1/trade/cancel-order";
inline constexpr std::string_view k_amend_order = "/api/v1/trade/amend-order";
inline constexpr std::string_view k_orders_pending = "/api/v1/trade/orders-pending";
inline constexpr std::string_view k_order_detail = "/api/v1/trade/order-detail";
inline constexpr std::string_view k_fills_history = "/api/v1/trade/fills-history";
inline constexpr std::string_view k_positions = "/api/v1/account/positions";
inline constexpr std::string_view k_balance = "/api/v1/account/balance";

inline constexpr std::int64_t k_unauthorized = 401;
inline constexpr std::int64_t k_no_such_key = 152401;
inline constexpr std::int64_t k_key_expired = 152402;
inline constexpr std::int64_t k_stale_timestamp = 152405;
inline constexpr std::int64_t k_ip_not_allowed = 152406;
inline constexpr std::int64_t k_repeated_nonce = 152407;
inline constexpr std::int64_t k_bad_passphrase = 152408;
inline constexpr std::int64_t k_bad_signature = 152409;
inline constexpr std::int64_t k_cancel_failed = 102068; // filled, cancelled or gone
inline constexpr std::int64_t k_too_many_requests = 429;
inline constexpr std::int64_t k_login_failed = 60009;

// unknown codes are terminal so an unknown refusal is not retried at full speed
[[nodiscard]] exec::ErrorClass classify(std::int64_t code, int http_status) noexcept;

[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

// base64 of the lowercase hex hmac, the hex taken as text
[[nodiscard]] std::string_view sign_prehash(const net::Signer& signer, std::string_view prehash, std::span<char> out) noexcept;

[[nodiscard]] std::optional<std::string>
login_frame(const net::Signer& signer, std::string_view passphrase, UnixNanos now);

[[nodiscard]] std::string_view subscribe_orders_frame() noexcept;

enum class BuildError : std::uint8_t { NoCredentials, BufferTooSmall, UnsupportedOrder, BadValue };

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// a request's views point into this builder; send it before building the next
class RequestBuilder {
public:
    RequestBuilder(const net::Signer& signer, const std::string& passphrase) noexcept
        : signer_{&signer}, passphrase_{&passphrase} {}

    // cross margin, net position mode: positionSide is always "net"
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, std::string_view inst_id, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> orders_pending(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    order_detail(std::string_view inst_id, const ExchangeClientOrderId& id, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> fills(UnixNanos since, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> balance(UnixNanos now);

    // exposed for tests: the bytes the last request signed
    [[nodiscard]] std::string_view last_prehash() const noexcept { return {prehash_.data(), prehash_len_}; }

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view target, std::string_view body, UnixNanos now);

    const net::Signer* signer_;
    const std::string* passphrase_;
    std::array<char, 512> body_{};
    std::array<char, 512> target_{};
    std::array<char, 1024> prehash_{};
    std::array<char, 24> timestamp_{};
    std::array<char, 40> nonce_{};
    std::array<char, 96> signature_{};
    std::size_t prehash_len_{0};
    std::uint64_t nonce_seq_{0};
};

// code is a string; an absent one reads as success
struct WireReply {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::optional<WireReply> parse_reply(std::string_view body);

// one order, from the orders channel or a rest list; fills are cumulative, never per trade
struct WireOrder {
    std::string_view inst_id;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view side;       // buy | sell
    std::string_view order_type; // limit | market | post_only | ioc | fok
    std::string_view state;      // live | partially_filled | filled | canceled | partially_canceled
    std::string_view price;
    std::string_view size;
    std::string_view filled_size;
    std::string_view average_price;
    std::string_view fee;
    std::uint64_t update_ms{0};
    bool reduce_only{false};
};

inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> rows{};
};

enum class PrivateKind : std::uint8_t { Login, LoginFailed, Orders, Snapshot, Other };

struct PrivateFrame {
    PrivateKind kind{PrivateKind::Other};
    std::span<const WireOrder> orders;
    std::int64_t code{0};
};

[[nodiscard]] std::expected<PrivateFrame, WireError> parse_private(std::string_view frame, OrderBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                               OrderBuffer& storage);

struct WireFill {
    std::string_view inst_id;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view side;
    std::string_view price;
    std::string_view size;
    std::string_view fee;
    std::uint64_t ts_ms{0};
};

struct FillBuffer {
    std::array<WireFill, k_max_orders> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                             FillBuffer& storage);

struct WirePosition {
    std::string_view inst_id;
    std::string_view positions; // signed contracts in net mode
    std::string_view average_price;
};

struct PositionBuffer {
    std::array<WirePosition, k_max_orders> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                     PositionBuffer& storage);

struct WireBalance {
    std::string_view currency;
    std::string_view balance;
    std::string_view available;
    std::string_view unrealized;
};

struct BalanceBuffer {
    std::array<WireBalance, 64> rows{};
};

[[nodiscard]] std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body,
                                                                                   BalanceBuffer& storage);

// what the orders channel has said about one order so far
struct OrderProgress {
    std::int64_t filled_lots{0};
    FixedE9 average_price{};
    FixedE9 fee{};
};

using ProgressTable = std::unordered_map<std::string, OrderProgress>;

// a push becomes a fill when the cumulative size grew, priced from the moved average, and a
// lifecycle step when the state changed; `seed_only` records progress from the snapshot silently
void map_order_push(const WireOrder& wire,
                    const exec::InstrumentTable& table,
                    ProgressTable& progress,
                    UnixNanos ts_init,
                    std::span<OrderEvent, 2> out,
                    std::size_t& count,
                    bool seed_only = false);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_order_report(const WireOrder& wire,
                                                                      const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos now);

} // namespace raikiri::venues::blofin
