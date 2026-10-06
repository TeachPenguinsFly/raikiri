#pragma once

// a fill push names only the venue's orderId, so its client id and leaves come from the order context

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace raikiri::venues::weex {

// the place answer, or the cancel answer with its final status
struct WireSpotAck {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view status;
};

// a body without orderId is unreadable; refusals are {"code","msg"} and go through parse_rest_error
[[nodiscard]] std::expected<WireSpotAck, WireError> parse_spot_ack(std::string_view body);

// one row of GET /api/v3/myTrades; it states neither side nor liquidity, only isBuyer
struct WireSpotTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view price;
    std::string_view qty;
    std::string_view commission;
    bool buyer{false};
    std::uint64_t time_ms{0};
};

// the venue's page maxes at 200
inline constexpr std::size_t k_max_spot_trades = 256;

struct SpotTradeBuffer {
    std::array<WireSpotTrade, k_max_spot_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError> parse_spot_trades(std::string_view body,
                                                                                         SpotTradeBuffer& storage);

// one asset of GET /api/v3/account
struct WireSpotBalance {
    std::string_view asset;
    std::string_view free;
    std::string_view locked;
};

inline constexpr std::size_t k_max_spot_balances = 2048;

struct SpotBalanceBuffer {
    std::array<WireSpotBalance, k_max_spot_balances> balances{};
};

[[nodiscard]] std::expected<std::span<const WireSpotBalance>, WireError> parse_spot_account(std::string_view body,
                                                                                            SpotBalanceBuffer& storage);

enum class SpotPrivateKind : std::uint8_t {
    Orders,  // e == "orders"
    Fill,    // e == "fill"
    Account, // e == "account"
    Ping,    // {"type":"ping","time":"..."}
    Ack,     // {"result":true,"id":n}
    Refusal, // {"result":false,"id":n,"msg":"..."}
    Unknown,
};

struct SpotPrivateInfo {
    SpotPrivateKind kind{SpotPrivateKind::Unknown};
    std::string_view msg;
};

[[nodiscard]] std::expected<SpotPrivateInfo, WireError> parse_spot_private(std::string_view frame);

// one entry of an orders push's d
struct WireSpotOrderPush {
    std::string_view order_id;
    std::string_view symbol;
    std::string_view side;
    std::string_view price;
    std::string_view size;
    std::string_view client_order_id;
    std::string_view type;
    std::string_view time_in_force;
    std::string_view status;
    std::string_view cum_fill_size;
    std::uint64_t updated_ms{0};
};

// one entry of a fill push's d; it carries value rather than price
struct WireSpotFillPush {
    std::string_view fill_id;
    std::string_view symbol;
    std::string_view order_id;
    std::string_view side;
    std::string_view fill_size;
    std::string_view fill_value;
    std::string_view fill_fee;
    std::string_view direction; // MAKER | TAKER
    std::uint64_t created_ms{0};
};

inline constexpr std::size_t k_max_spot_pushes = 64;

struct SpotOrderPushBuffer {
    std::array<WireSpotOrderPush, k_max_spot_pushes> orders{};
};

struct SpotFillPushBuffer {
    std::array<WireSpotFillPush, k_max_spot_pushes> fills{};
};

[[nodiscard]] std::expected<std::span<const WireSpotOrderPush>, WireError>
parse_spot_order_push(std::string_view frame, SpotOrderPushBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireSpotFillPush>, WireError>
parse_spot_fill_push(std::string_view frame, SpotFillPushBuffer& storage);

// the page's example prices a buy's commission in quote, so it is taken as it comes
[[nodiscard]] std::optional<exec::FillReport> map_spot_trade(const WireSpotTrade& wire,
                                                             const exec::InstrumentTable& table);

// total is free plus locked
[[nodiscard]] std::optional<BalanceReport> map_spot_balance(const WireSpotBalance& wire, UnixNanos ts_venue);

// what is known of one venue order id
struct SpotOrderContext {
    ExchangeClientOrderId client_order_id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    QtyLots qty{QtyLots::undef()};
    // summed from fill pushes only, so an order push arriving first or last counts nothing twice
    QtyLots filled{QtyLots::from_raw(0)};
};

// working, canceled, expired and rejected states; fills come from the fill channel
[[nodiscard]] std::optional<OrderEvent>
map_spot_order_push(const WireSpotOrderPush& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// leaves is the context's qty less what it has filled including this one; undef when qty is unknown
// the channel's example charges a buy in base, so that fee is converted at the fill price
[[nodiscard]] std::optional<OrderEvent> map_spot_fill_push(const WireSpotFillPush& wire,
                                                           const exec::InstrumentTable& table,
                                                           const SpotOrderContext& context,
                                                           UnixNanos ts_init);

enum class SpotVerdictKind : std::uint8_t {
    Accepted,
    Canceled,
    Refused,
    // the venue may or may not have the order; reconciliation decides, nothing is resent
    Unknown,
};

struct SpotVerdict {
    SpotVerdictKind kind{SpotVerdictKind::Refused};
    exec::VenueError error{};
    std::string_view order_id;
};

// the rest answer to one create or cancel; a 5xx, -1000, -1054 or unreadable create is Unknown
[[nodiscard]] SpotVerdict spot_verdict(bool cancel, int status, std::string_view body) noexcept;

// one channel per frame
inline constexpr std::array<std::string_view, 3> k_spot_private_subscribes = {
    R"({"method":"SUBSCRIBE","params":["orders"],"id":1})",
    R"({"method":"SUBSCRIBE","params":["fill"],"id":2})",
    R"({"method":"SUBSCRIBE","params":["account"],"id":3})",
};

inline constexpr std::uint32_t k_spot_private_stream_id = 2;

using StreamHeaders = std::vector<std::pair<std::string, std::string>>;

// the socket signs timestamp + ep::k_spot_private_ws in handshake headers; empty without credentials
[[nodiscard]] StreamHeaders spot_stream_headers(const net::Signer& signer, std::string_view passphrase, UnixNanos now);

// routes private frames to the sink; owns the order contexts so a fill finds its client id
class SpotPrivateFeed {
public:
    SpotPrivateFeed(exec::GatewaySink& sink, const exec::InstrumentTable& table, const Clock& clock) noexcept
        : sink_{&sink}, table_{&table}, clock_{&clock} {}

    // a rest ack or an order push names the order id
    void remember(std::string_view order_id, const SpotOrderContext& context);

    // the reply owed to the frame, a pong to a ping
    [[nodiscard]] std::optional<std::string_view> on_frame(std::string_view frame, UnixNanos received) noexcept;

    void on_close() noexcept;

    // symbols this process has traded or seen working, since myTrades takes one symbol per call
    void note_symbol(std::string_view symbol);

    [[nodiscard]] const std::unordered_set<std::string>& symbols() const noexcept { return symbols_; }

    [[nodiscard]] std::size_t contexts() const noexcept { return contexts_.size(); }

private:
    void on_orders(std::string_view frame, UnixNanos received) noexcept;
    void on_fills(std::string_view frame, UnixNanos received) noexcept;
    void connectivity(exec::ConnState state) noexcept;

    exec::GatewaySink* sink_;
    const exec::InstrumentTable* table_;
    const Clock* clock_;
    std::unordered_map<std::string, SpotOrderContext> contexts_;
    std::unordered_set<std::string> symbols_;
    std::size_t acks_{0};
};

} // namespace raikiri::venues::weex
