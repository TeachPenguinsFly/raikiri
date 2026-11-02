#pragma once

// the public surface's rows; prices, quantities, ids and nanosecond times stay strings

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::clientapi {

struct NewOrder {
    std::string account{};
    std::string instrument_id{};
    std::string side{};
    std::string order_type{};
    std::string tif{"gtc"};
    std::optional<std::string> price{};
    std::string qty{};
    bool post_only{false};
    bool reduce_only{false};
    // set here from the key, never taken from a client
    std::optional<std::string> book{};
    std::optional<std::string> external_ref{};
};

inline constexpr std::size_t k_max_external_ref = 40;

// the same shape as a refused call's problem+json; a denied order carries it, as strategyd decides after the 202
struct OrderRefusal {
    std::string type{};
    std::string title{};
    std::int32_t status{0};
    std::string code{};
};

struct AmendOrder {
    std::optional<std::string> price{};
    std::optional<std::string> qty{};
};

struct OrderRow {
    std::string client_order_id{};
    std::optional<std::string> exchange_order_id{};
    std::string instrument_id{};
    std::string side{};
    std::string order_type{};
    std::string tif{};
    std::optional<bool> post_only{};
    std::optional<bool> reduce_only{};
    std::optional<std::string> price{};
    std::string qty{};
    std::optional<std::string> leaves{};
    std::string status{};
    std::optional<std::string> reject_class{};
    std::optional<std::int64_t> reject_code{};
    std::string ts_created{};
    std::string ts_updated{};
    std::optional<std::string> book{};
    std::optional<std::string> external_ref{};
    // filled in here; the backend knows neither
    std::optional<std::string> client_id{};
    std::optional<std::string> account{};
    std::optional<std::string> symbol{};
    std::optional<std::string> parent_id{};
    std::optional<OrderRefusal> refusal{};
};

struct OrderEventRow {
    std::string client_order_id{};
    std::string instrument_id{};
    std::string event_type{};
    std::optional<std::string> price{};
    std::optional<std::string> qty{};
    std::optional<std::string> leaves{};
    std::optional<std::string> fee{};
    std::optional<std::string> exec_id{};
    std::string ts_event{};
    std::optional<std::string> client_id{};
    std::optional<std::string> account{};
    std::optional<std::string> symbol{};
    std::optional<std::string> parent_id{};
};

struct FillRow {
    std::string venue{};
    std::string exec_id{};
    std::optional<std::string> client_order_id{};
    std::optional<std::string> exchange_order_id{};
    std::string instrument_id{};
    std::string side{};
    std::string price{};
    std::string qty{};
    std::optional<std::string> fee{};
    std::optional<std::string> liquidity{};
    std::string ts_event{};
    std::optional<std::string> external_ref{};
    std::optional<std::string> client_id{};
    std::optional<std::string> account{};
    std::optional<std::string> symbol{};
    std::optional<std::string> parent_id{};
};

struct PositionRow {
    std::string client_id{};
    std::string account{};
    std::string symbol{};
    std::string instrument_id{};
    std::string qty{}; // signed; positive is long
    std::optional<std::string> avg_entry_price{};
    std::string realized{};
    std::string fees{};
    std::string ts_updated{};
};

struct BalanceRow {
    std::string asset{};
    std::optional<std::string> total{};
    std::optional<std::string> available{};
    std::optional<std::string> unrealized{};

    friend bool operator==(const BalanceRow&, const BalanceRow&) = default;
};

struct AccountBalances {
    std::string account{};
    std::string venue{};
    std::optional<std::string> reported_at{};
    bool stale{false};
    std::optional<std::string> wallet{};
    std::vector<BalanceRow> assets{};
};

struct InstrumentRow {
    std::string instrument_id{};
    std::string account{};
    std::string venue{};
    std::string market{};
    std::string exchange_symbol{};
    std::string local_symbol{};
    std::string tick_size{};
    std::string lot_size{};
    std::string min_notional{};
    std::string contract_value{};
    std::string contract_kind{};
    std::string asset_class{};
    std::optional<std::string> asset_key{};
    std::optional<std::string> expiry{};
    std::optional<std::string> strike{};
    std::optional<std::string> put_call{};
};

struct ListingRow {
    std::string instrument_id{};
    std::string account{};
    std::string venue{};
    std::string market{};
    std::string exchange_symbol{};
    std::string local_symbol{};
};

struct MarketRow {
    std::string key{};
    std::string symbol{};
    std::vector<ListingRow> listings{};
};

struct AccountRow {
    std::string account{};
    std::string venue{};
    std::string market{};
    bool alive{false};
};

struct NewParent {
    std::string symbol{};
    std::string market{"perp"};
    std::string side{};
    std::optional<std::string> qty{};
    std::optional<std::string> notional{};
    std::optional<std::string> limit{};
    std::string algo{"twap"};
    std::optional<std::uint64_t> horizon_s{};
    std::optional<std::uint32_t> slices{};
    std::optional<std::uint32_t> jitter_bp{};
    std::optional<std::uint32_t> catchup_pct{};
    std::optional<std::string> profile{};
    std::optional<std::uint32_t> pov_bp{};
    std::optional<std::string> max_child_notional{};
    // set here from the key, never taken from a client
    std::optional<std::string> book{};
    std::optional<std::string> external_ref{};
};

struct ChildRow {
    std::string client_order_id{};
    std::string instrument_id{};
    std::string venue{};
    std::string qty{};
    std::string limit{};
    std::string filled{};
    std::optional<std::string> avg_price{};
    std::string fees{};
    std::string sent{};
    std::optional<std::string> acked{};
    std::optional<std::string> closed{};
    std::string end{};
    bool open{false};
};

struct ParentRow {
    std::string id{};
    std::string symbol{};
    std::string market{};
    std::string side{};
    std::string algo{};
    std::string profile{};
    std::string status{};
    std::optional<std::string> reason{};
    std::string qty{};
    std::string filled{};
    std::string working{};
    std::optional<std::string> limit{};
    std::optional<std::string> arrival{};
    std::optional<std::string> avg_price{};
    std::optional<std::string> market_vwap{};
    std::optional<std::string> slippage_arrival_bp{};
    std::optional<std::string> slippage_vwap_bp{};
    std::string fees{};
    std::int64_t horizon_s{0};
    std::int64_t slices{0};
    std::int64_t slices_fired{0};
    std::int64_t pov_bp{0};
    std::string created{};
    std::optional<std::string> started{};
    std::optional<std::string> ended{};
    std::int64_t child_count{0};
    std::optional<std::vector<ChildRow>> children{};
    std::optional<std::string> book{};
    std::optional<std::string> external_ref{};
    std::optional<std::string> client_id{};
};

template <typename T> struct Page {
    std::vector<T> items{};
    std::optional<std::string> next_cursor{};
};

[[nodiscard]] inline bool order_is_terminal(std::string_view status) noexcept {
    return status == "filled" || status == "canceled" || status == "rejected" || status == "expired" ||
           status == "denied";
}

[[nodiscard]] inline bool parent_is_terminal(std::string_view status) noexcept {
    return status == "done" || status == "cancelled" || status == "failed";
}

} // namespace raikiri::clientapi
