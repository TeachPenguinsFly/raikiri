#pragma once

// the account side: tx verdicts, order and trade pushes, the account row; views live until the next parse

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/error_class.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/lighter/errors.hpp>

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace raikiri::venues::lighter {

// a websocket answer carries our id and either a verdict or {"error":{code,message}}; rest has no id
struct SendReply {
    std::string_view id{};
    std::int64_t code{0};
    std::string_view message{};
    std::string_view tx_hash{};

    [[nodiscard]] bool ok() const noexcept { return code == 200; }
};

[[nodiscard]] std::expected<SendReply, WireError> parse_send_reply(std::string_view frame);

[[nodiscard]] std::expected<std::int64_t, WireError> parse_next_nonce(std::string_view body);

struct WireOrder {
    std::uint64_t order_index{0};
    std::int64_t client_order_index{0};
    std::uint32_t market_index{0};
    bool is_ask{false};
    std::string_view status{};
    std::string_view price{};
    std::string_view initial_base_amount{};
    std::string_view remaining_base_amount{};
    std::string_view filled_base_amount{};
    std::uint64_t updated_ms{0};
};

struct WireTrade {
    std::uint64_t trade_id{0};
    std::uint32_t market_id{0};
    std::string_view price{};
    std::string_view size{};
    std::uint64_t ask_id{0};
    std::uint64_t bid_id{0};
    std::int64_t ask_client_id{0};
    std::int64_t bid_client_id{0};
    std::int64_t ask_account_id{0};
    std::int64_t bid_account_id{0};
    bool is_maker_ask{false};
    std::string_view usd_amount{};
    // fee rates in millionths of the notional; the venue omits a zero one
    std::uint64_t taker_fee{0};
    std::uint64_t maker_fee{0};
    std::uint64_t ts_ms{0};
};

// the side's fee in usdc, rounded toward zero; undefined when the notional does not read
[[nodiscard]] FixedE9 fee_of(const WireTrade& trade, bool maker) noexcept;

struct WirePosition {
    std::uint32_t market_id{0};
    std::int64_t sign{0};
    std::string_view position{};
    std::string_view avg_entry_price{};
    std::string_view unrealized_pnl{};
};

struct WireAccount {
    std::string_view collateral{};
    std::string_view available_balance{};
    std::vector<WirePosition> positions;
};

// rest lists orders in an array, the stream keys them by market; both land here
[[nodiscard]] std::expected<void, WireError> parse_account_orders(std::string_view frame, std::vector<WireOrder>& out);
[[nodiscard]] std::expected<void, WireError> parse_account_trades(std::string_view frame, std::vector<WireTrade>& out);
[[nodiscard]] std::expected<void, WireError> parse_account(std::string_view body, WireAccount& out);

enum class OrderPhase : std::uint8_t { Live, Filled, Canceled, PostOnlyCrossed, Unknown };

// "canceled-*" carries the reason; a post-only that would cross is a refusal, not a cancel
[[nodiscard]] OrderPhase order_phase(std::string_view status) noexcept;

[[nodiscard]] ErrorClass classify_code(std::int64_t code) noexcept;

inline constexpr std::int64_t k_code_ok = 200;
inline constexpr std::int64_t k_code_invalid_nonce = 21104;
inline constexpr std::int64_t k_code_bad_attributes = 21518;

// the low 48 bits of the ulid; within one millisecond the oms increments them, so they differ
[[nodiscard]] std::int64_t client_order_index(const ExchangeClientOrderId& id) noexcept;

} // namespace raikiri::venues::lighter
