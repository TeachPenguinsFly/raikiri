#pragma once

#include <raikiri/sdk/decimal.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::sdk {

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { Gtc, Ioc, Fok };

// Unknown is a status newer than this library
enum class OrderStatus : std::uint8_t {
    Unknown,
    Initialized,
    Denied,
    Submitted,
    Accepted,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected,
    Expired,
    Triggered,
    PendingUpdate,
    PendingCancel,
};

[[nodiscard]] std::string_view to_string(Side side) noexcept;
[[nodiscard]] std::string_view to_string(OrderStatus status) noexcept;

// unix nanoseconds; on the wire as digits because they do not survive a double
struct Nanos {
    std::uint64_t value{0};

    friend constexpr auto operator<=>(Nanos, Nanos) noexcept = default;
};

template <typename T> struct Page {
    std::vector<T> items{};
    std::optional<std::string> next_cursor{};
};

struct Instrument {
    std::string instrument_id{};
    std::string account{};
    std::string venue{};
    std::string market{};
    std::string exchange_symbol{};
    std::string local_symbol{};
    Decimal tick_size{};
    Decimal lot_size{};
    Decimal min_notional{};
    Decimal contract_value{};
    std::string contract_kind{};
    std::string asset_class{};
    std::optional<std::string> asset_key{};
    std::optional<Nanos> expiry{};
    std::optional<Decimal> strike{};
    std::optional<std::string> put_call{};
};

struct Listing {
    std::string instrument_id{};
    std::string account{};
    std::string venue{};
    std::string market{};
    std::string exchange_symbol{};
    std::string local_symbol{};
};

struct Market {
    std::string key{};
    std::string symbol{};
    std::vector<Listing> listings{};
};

struct Account {
    std::string account{};
    std::string venue{};
    std::string market{};
    bool alive{false};
};

struct OrderAck {
    std::string client_order_id{};
    std::string status{};
    std::string client_id{};
    std::optional<std::string> replaces{};
};

struct CancelAck {
    std::string client_order_id{};
    std::string status{};
};

struct CancelAll {
    std::vector<std::string> canceled{};
    std::vector<std::string> failed{};
};

// why a denied order was refused, in the shape of a refused call's problem; `code` is stable
struct OrderRefusal {
    std::string code{};
    std::string title{};
    std::int32_t status{0};
    std::string type{};
};

struct Order {
    std::string client_order_id{};
    std::string instrument_id{};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Gtc};
    Decimal qty{};
    OrderStatus status{OrderStatus::Unknown};
    Nanos ts_created{};
    Nanos ts_updated{};
    std::optional<std::string> exchange_order_id{};
    std::optional<bool> post_only{};
    std::optional<bool> reduce_only{};
    std::optional<Decimal> price{};
    std::optional<Decimal> leaves{};
    std::optional<std::string> reject_class{};
    std::optional<std::int64_t> reject_code{};
    std::optional<std::string> client_id{};
    std::optional<std::string> account{};
    std::optional<std::string> symbol{};
    std::optional<std::string> parent_id{};
    std::optional<std::string> book{};
    std::optional<std::string> external_ref{};
    std::optional<OrderRefusal> refusal{};

    [[nodiscard]] bool is_open() const noexcept {
        switch (status) {
        case OrderStatus::Denied:
        case OrderStatus::Filled:
        case OrderStatus::Canceled:
        case OrderStatus::Rejected:
        case OrderStatus::Expired:
            return false;
        default:
            return true;
        }
    }

    [[nodiscard]] Decimal filled() const { return qty - leaves.value_or(qty); }
};

struct OrderEvent {
    std::string client_order_id{};
    std::string instrument_id{};
    std::string event_type{};
    Nanos ts_event{};
    std::optional<Decimal> price{};
    std::optional<Decimal> qty{};
    std::optional<Decimal> leaves{};
    std::optional<Decimal> fee{};
    std::optional<std::string> exec_id{};
    std::optional<std::string> client_id{};
    std::optional<std::string> account{};
    std::optional<std::string> symbol{};
    std::optional<std::string> parent_id{};
};

struct Fill {
    std::string venue{};
    std::string exec_id{};
    std::string instrument_id{};
    Side side{Side::Buy};
    Decimal price{};
    Decimal qty{};
    Nanos ts_event{};
    std::optional<std::string> client_order_id{};
    std::optional<std::string> exchange_order_id{};
    std::optional<Decimal> fee{};
    std::optional<std::string> liquidity{};
    std::optional<std::string> client_id{};
    std::optional<std::string> account{};
    std::optional<std::string> symbol{};
    std::optional<std::string> parent_id{};
};

struct Position {
    std::string client_id{};
    std::string account{};
    std::string symbol{};
    std::string instrument_id{};
    Decimal qty{}; // signed; positive is long
    Decimal realized{};
    Decimal fees{};
    Nanos ts_updated{};
    std::optional<Decimal> avg_entry_price{};
};

struct Balance {
    std::string asset{};
    std::optional<Decimal> total{};
    std::optional<Decimal> available{};
    std::optional<Decimal> unrealized{};
};

struct AccountBalances {
    std::string account{};
    std::string venue{};
    bool stale{false};
    std::optional<Nanos> reported_at{};
    std::optional<std::string> wallet{};
    std::vector<Balance> assets{};
};

struct ChildOrder {
    std::string client_order_id{};
    std::string instrument_id{};
    std::string venue{};
    Decimal qty{};
    Decimal limit{};
    Decimal filled{};
    Decimal fees{};
    Nanos sent{};
    std::string end{};
    bool open{false};
    std::optional<Decimal> avg_price{};
    std::optional<Nanos> acked{};
    std::optional<Nanos> closed{};
};

struct AlgoOrder {
    std::string id{};
    std::string symbol{};
    std::string market{};
    Side side{Side::Buy};
    std::string algo{};
    std::string profile{};
    std::string status{};
    Decimal qty{};
    Decimal filled{};
    Decimal working{};
    Decimal fees{};
    std::int64_t horizon_s{0};
    std::int64_t slices{0};
    std::int64_t slices_fired{0};
    std::int64_t pov_bp{0};
    Nanos created{};
    std::int64_t child_count{0};
    std::optional<std::string> reason{};
    std::optional<Decimal> limit{};
    std::optional<Decimal> arrival{};
    std::optional<Decimal> avg_price{};
    std::optional<Decimal> market_vwap{};
    std::optional<Decimal> slippage_arrival_bp{};
    std::optional<Decimal> slippage_vwap_bp{};
    std::optional<Nanos> started{};
    std::optional<Nanos> ended{};
    std::optional<std::vector<ChildOrder>> children{};
    std::optional<std::string> client_id{};
    std::optional<std::string> book{};
    std::optional<std::string> external_ref{};

    [[nodiscard]] bool done() const noexcept { return status == "done" || status == "cancelled" || status == "failed"; }

    // filled share of the target, 0 to 1; zero until the target is known
    [[nodiscard]] Decimal progress() const { return qty > Decimal{} ? filled / qty : Decimal{}; }
};

struct BookLevel {
    Decimal price{};
    Decimal size{};
};

struct Quote {
    std::string instrument_id{};
    Decimal bid{};
    Decimal bid_size{};
    Decimal ask{};
    Decimal ask_size{};
    Nanos ts_event{};

    [[nodiscard]] Decimal mid() const { return (bid + ask) / Decimal{2}; }
};

struct Book {
    std::string instrument_id{};
    std::vector<BookLevel> bids{};
    std::vector<BookLevel> asks{};
    Nanos ts_event{};
};

struct Greeks {
    std::string instrument_id{};
    Decimal delta{};
    Decimal gamma{};
    Decimal vega{};
    Decimal theta{};
    Decimal rho{};
    Decimal mark_iv{};
    Decimal bid_iv{};
    Decimal ask_iv{};
    Decimal underlying_price{};
    Decimal mark_price{};
    Decimal index_price{};
    Nanos ts_event{};
};

struct Funding {
    std::string instrument_id{};
    Decimal rate{};
    Decimal predicted_rate{};
    Decimal mark_price{};
    Nanos next_funding_time{};
    Nanos ts_event{};
    std::optional<Decimal> index_price{};
    std::optional<std::int64_t> interval_seconds{};
    std::optional<std::string> accrual{};
};

struct Ticker {
    std::string instrument_id{};
    Decimal volume_24h_base{};
    Decimal volume_24h_quote{};
    Nanos ts_event{};
    std::optional<Decimal> open_interest{};
};

struct AlgoLost {
    std::string id{};
    std::string reason{};
};

// events were lost; refetch orders, fills and positions over rest
struct Gap {
    std::string reason{};
    std::optional<std::uint64_t> from_seq{};
    std::optional<std::uint64_t> to_seq{};
};

struct OptionQuote {
    std::string venue{};
    std::string instrument_id{};
    std::string symbol{};
    std::string premium_unit{};
    std::string iv_source{};
    std::string greeks_source{};
    Nanos ts_event{};
    std::optional<Decimal> bid{};
    std::optional<Decimal> ask{};
    std::optional<Decimal> bid_size{};
    std::optional<Decimal> ask_size{};
    std::optional<Decimal> mark{};
    std::optional<Decimal> bid_usd{};
    std::optional<Decimal> ask_usd{};
    std::optional<Decimal> mark_usd{};
    std::optional<Decimal> bid_iv{};
    std::optional<Decimal> ask_iv{};
    std::optional<Decimal> mark_iv{};
    std::optional<Decimal> delta{};
    std::optional<Decimal> gamma{};
    std::optional<Decimal> vega{};
    std::optional<Decimal> theta{};
    std::optional<std::string> greeks_unit{};
    std::optional<Decimal> open_interest{};
    std::optional<Decimal> volume_24h{};
};

struct OptionBest {
    std::string venue{};
    Decimal usd{};
    std::optional<Decimal> iv{};
    std::optional<Decimal> size{};
};

struct OptionSide {
    std::vector<OptionQuote> venues{};
    std::optional<OptionBest> best_bid{};
    std::optional<OptionBest> best_ask{};
};

struct OptionRow {
    Decimal strike{};
    OptionSide call{};
    OptionSide put{};
};

struct OptionChain {
    std::string underlying{};
    Nanos expiry{};
    std::string date{};
    Nanos as_of{};
    std::int64_t seconds_to_expiry{0};
    std::string forward_source{};
    std::vector<OptionRow> rows{};
    std::optional<Decimal> years{};
    std::optional<Decimal> forward{};
    std::optional<Decimal> atm_strike{};
};

struct OptionExpiry {
    Nanos expiry{};
    std::string date{};
    std::int64_t strikes{0};
    std::vector<std::string> venues{};
};

struct OptionVenue {
    std::string venue{};
    std::int64_t options{0};
};

struct OptionUnderlying {
    std::string underlying{};
    std::vector<OptionVenue> venues{};
    std::vector<OptionExpiry> expiries{};
};

struct Rate {
    std::int64_t requests_per_second{0};
    std::int64_t burst{0};
    std::int64_t mutations_per_second{0};
    std::int64_t mutation_burst{0};
};

struct Whoami {
    std::string key_id{};
    std::string client_id{};
    std::vector<std::string> permissions{};
    std::vector<std::string> accounts{};
    std::vector<std::string> symbols{};
    Rate rate{};
    std::int64_t max_streams{0};
};

} // namespace raikiri::sdk
