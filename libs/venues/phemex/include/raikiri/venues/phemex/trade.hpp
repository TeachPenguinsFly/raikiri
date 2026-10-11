#pragma once

// usdt perpetual order entry on the hedged-perpetual api: signed rest out, the aop_p socket back.
// every request signs HMacSha256(path + query + expiry + body) in lowercase hex with the raw secret

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/error.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/phemex/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace raikiri::venues::phemex {

inline constexpr std::string_view k_create_order = "/g-orders/create";
inline constexpr std::string_view k_replace_order = "/g-orders/replace";
inline constexpr std::string_view k_cancel_order = "/g-orders/cancel";
inline constexpr std::string_view k_active_orders = "/g-orders/activeList";
inline constexpr std::string_view k_order_by_id = "/api-data/g-futures/orders/by-order-id";
inline constexpr std::string_view k_user_trades = "/api-data/g-futures/trades";
inline constexpr std::string_view k_account_positions = "/g-accounts/accountPositions";

// a request is refused once its expiry passes; the docs suggest a minute
inline constexpr std::uint64_t k_expiry_s = 60;

inline constexpr std::int64_t k_ok = 0;
inline constexpr std::int64_t k_unauthorized = 401;
inline constexpr std::int64_t k_duplicate_order = 10001;
inline constexpr std::int64_t k_order_not_found = 10002;
inline constexpr std::int64_t k_signature_failed = 10500;
inline constexpr std::int64_t k_no_balance = 11001;
inline constexpr std::int64_t k_duplicate_client_id = 11085;
inline constexpr std::int64_t k_request_duplicated = 19999;
inline constexpr std::int64_t k_too_many_requests = 39995;
inline constexpr std::int64_t k_access_denied = 39996;
inline constexpr std::int64_t k_invalid_login_token = 6012;

// unknown codes are terminal so an unknown refusal is not retried at full speed
[[nodiscard]] exec::ErrorClass classify(std::int64_t code, int http_status) noexcept;

// a cancel of an order already gone; reconciliation settles it
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

enum class BuildError : std::uint8_t { NoCredentials, BufferTooSmall, UnsupportedOrder, BadValue };

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view sign_request(const net::Signer& signer,
                                            std::string_view path,
                                            std::string_view query,
                                            std::uint64_t expiry_s,
                                            std::string_view body,
                                            std::span<char> out) noexcept;

// the socket's user.auth: HMacSha256(api key + expiry)
[[nodiscard]] std::optional<std::string> auth_frame(const net::Signer& signer, UnixNanos now, std::uint64_t id);

[[nodiscard]] std::string subscribe_frame(std::uint64_t id);

// a request's views point into this builder; send it before building the next
class RequestBuilder {
public:
    explicit RequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    // one-way mode: every order rides posSide=Merged
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, std::string_view symbol, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);
    // the venue lists open orders one symbol at a time
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(std::string_view symbol, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(std::string_view symbol, const ExchangeClientOrderId& id, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    user_trades(std::string_view symbol, UnixNanos since, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> account_positions(UnixNanos now);

    // exposed for tests: the bytes the last request signed
    [[nodiscard]] std::string_view last_signed() const noexcept { return {signed_.data(), signed_len_}; }

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view path, std::size_t query_len, UnixNanos now);

    const net::Signer* signer_;
    std::array<char, 512> query_{};
    std::array<char, 768> target_{};
    std::array<char, 768> signed_{};
    std::array<char, 24> expiry_{};
    std::array<char, net::k_hex_bytes + 1> signature_{};
    std::size_t signed_len_{0};
};

// code is a number on refusals and a string on auth failures
struct WireReply {
    std::int64_t code{0};
    std::string_view msg;
};

// an empty body or one without a code reads as success
[[nodiscard]] std::optional<WireReply> parse_reply(std::string_view body);

// one order row: rest rows carry Rq/Rp suffixes, socket rows bare quantities
struct WireOrderRow {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view side;          // Buy | Sell
    std::string_view order_type;    // Limit | Market | ...
    std::string_view time_in_force; // GoodTillCancel | PostOnly | ImmediateOrCancel | FillOrKill
    std::string_view status;        // ordStatus: New | PartiallyFilled | Filled | Canceled | Rejected | ...
    std::string_view action;        // New | Replace | Cancel | ...
    std::string_view exec_status;   // TakerFill | MakerFill on a fill
    std::string_view exec_id;
    std::string_view exec_qty;
    std::string_view exec_price;
    std::string_view exec_fee;
    std::string_view qty;
    std::string_view cum_qty;
    std::string_view leaves_qty;
    std::string_view price;
    std::uint64_t transact_ns{0};
    bool reduce_only{false};
};

inline constexpr std::size_t k_max_order_rows = 512;

struct OrderRowBuffer {
    std::array<WireOrderRow, k_max_order_rows> rows{};
};

// a bare array, {"data":[..]} or {"data":{"rows":[..]}}; the docs show all three
[[nodiscard]] std::expected<std::span<const WireOrderRow>, WireError> parse_order_rows(std::string_view body,
                                                                                       OrderRowBuffer& storage);

enum class PrivateKind : std::uint8_t {
    Orders,   // an incremental aop_p push
    Snapshot, // the aop_p snapshot sent on subscribe
    Reply,    // the answer to user.auth, aop_p.subscribe or server.ping
    Other,
};

struct PrivateFrame {
    PrivateKind kind{PrivateKind::Other};
    std::span<const WireOrderRow> orders;
    std::int64_t error_code{0}; // on a reply; zero means success
    std::uint64_t id{0};
};

[[nodiscard]] std::expected<PrivateFrame, WireError> parse_private(std::string_view frame, OrderRowBuffer& storage);

struct WirePosition {
    std::string_view symbol;
    std::string_view side; // Buy | Sell | None
    std::string_view size;
    std::string_view avg_entry_price;
};

struct WireAccount {
    std::string_view currency;
    std::string_view balance;
    std::string_view used;
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> rows{};
};

struct WireAccountPositions {
    WireAccount account;
    std::span<const WirePosition> positions;
};

[[nodiscard]] std::expected<WireAccountPositions, WireError> parse_account_positions(std::string_view body,
                                                                                     PositionBuffer& storage);

// a fill row becomes Filled; anything else the lifecycle step its status names, or nothing
[[nodiscard]] std::optional<OrderEvent>
map_order_row(const WireOrderRow& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_order_report(const WireOrderRow& wire,
                                                                      const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill_report(const WireOrderRow& wire,
                                                              const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

[[nodiscard]] std::optional<BalanceReport> map_balance(const WireAccount& wire, UnixNanos now);

} // namespace raikiri::venues::phemex
