#pragma once

#include <raikiri/audit/log.hpp>
#include <raikiri/clientapi/domain/guards.hpp>
#include <raikiri/clientapi/domain/key.hpp>
#include <raikiri/clientapi/domain/trading.hpp>
#include <raikiri/common/time.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::clientapi {

using AuditEntry = audit::Entry;
using AuditSink = audit::Sink;

class KeyDirectory {
public:
    KeyDirectory() = default;
    KeyDirectory(const KeyDirectory&) = delete;
    KeyDirectory& operator=(const KeyDirectory&) = delete;
    virtual ~KeyDirectory() = default;

    [[nodiscard]] virtual std::shared_ptr<const ApiKey> find(std::string_view id) const = 0;

    // constant time; lowercase hex of hmac-sha256 under the key's secret
    [[nodiscard]] virtual bool
    verify(const ApiKey& key, std::string_view message, std::string_view signature) const = 0;
};

// NotDelivered is proof the call did nothing; NoAnswer is not
enum class BackendFailure : std::uint8_t { Answered, NotDelivered, NoAnswer };

struct BackendError {
    BackendFailure failure{BackendFailure::Answered};
    int status{0};
    std::string title{};
    std::string detail{};
};

template <typename T> using BackendResult = std::expected<T, BackendError>;

struct OrderQuery {
    std::string account{};
    std::string instrument_id{};
    std::string since{};
    std::string cursor{};
    std::size_t limit{100};
    std::optional<bool> open{};
    std::string book{};
};

class Backend {
public:
    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    virtual ~Backend() = default;

    // opening one that exists is not an error
    [[nodiscard]] virtual BackendResult<void> open_book(std::string_view name) = 0;
    [[nodiscard]] virtual BackendResult<std::string> place_order(const NewOrder& order, std::string_view key) = 0;
    [[nodiscard]] virtual BackendResult<void> cancel_order(std::string_view id) = 0;
    [[nodiscard]] virtual BackendResult<OrderRow> order(std::string_view id) = 0;
    [[nodiscard]] virtual BackendResult<std::vector<OrderEventRow>> order_events(std::string_view id) = 0;
    [[nodiscard]] virtual BackendResult<Page<OrderRow>> orders(const OrderQuery& query) = 0;
    [[nodiscard]] virtual BackendResult<Page<FillRow>> fills(std::string_view since, std::string_view cursor) = 0;
    [[nodiscard]] virtual BackendResult<InstrumentRow> instrument(std::string_view id) = 0;
    [[nodiscard]] virtual BackendResult<Page<InstrumentRow>>
    instruments(std::string_view account, std::string_view kind, std::string_view cursor, std::size_t limit) = 0;
    [[nodiscard]] virtual BackendResult<std::vector<MarketRow>> markets(std::string_view query, std::size_t limit) = 0;
    [[nodiscard]] virtual BackendResult<std::vector<AccountRow>> accounts() = 0;
    [[nodiscard]] virtual BackendResult<std::vector<AccountBalances>> balances() = 0;
    [[nodiscard]] virtual BackendResult<std::string> start_parent(const NewParent& parent) = 0;
    [[nodiscard]] virtual BackendResult<ParentRow> parent(std::string_view id) = 0;
    [[nodiscard]] virtual BackendResult<ParentRow> parent_action(std::string_view id, std::string_view action) = 0;
    // read-only and passed through as it is: the option chain and surface have no client-owned field
    [[nodiscard]] virtual BackendResult<std::string> read_raw(std::string_view target) = 0;
    [[nodiscard]] virtual BackendResult<std::string> stream_token() = 0;
};

// one extension the owner listed: a loopback service behind /api/v1/x/<name>/
struct ExtensionRoute {
    std::string method{};
    std::string path{}; // * stands for one segment
};

struct Extension {
    std::string name{};
    std::uint16_t port{0};
    Permission permission{Permission::Trade};
    std::string client_field{"client"};
    std::vector<ExtensionRoute> routes{};
    // A multi-account extension must be reachable only by keys permitted to use every account it can trade.
    std::vector<std::string> required_accounts{};
    // Domain catalogues that do not use raikiri symbols can require "*" until their own market ACL is available.
    std::vector<std::string> required_symbols{};
};

struct RawReply {
    int status{0};
    std::string body{};
};

class ExtensionClient {
public:
    ExtensionClient() = default;
    ExtensionClient(const ExtensionClient&) = delete;
    ExtensionClient& operator=(const ExtensionClient&) = delete;
    virtual ~ExtensionClient() = default;

    [[nodiscard]] virtual std::expected<RawReply, BackendFailure>
    call(std::uint16_t port, std::string_view method, std::string_view target, std::string_view body) = 0;
};

struct OwnedOrder {
    std::string id{};
    std::string client_id{};
    std::string key_id{};
    std::string account{};
    std::string symbol{};
    std::string instrument_id{};
    std::string side{};
    std::string order_type{};
    std::string tif{};
    std::optional<std::string> price{};
    std::string qty{};
    bool post_only{false};
    bool reduce_only{false};
    std::optional<std::string> parent_id{};
    std::string ts{};
};

struct OwnedParent {
    std::string id{};
    std::string client_id{};
    std::string key_id{};
    std::string symbol{};
    std::string created{};
    bool closed{false};
};

struct InstrumentScale {
    std::string tick_size{};
    std::string lot_size{};
    std::string contract_value{};
    std::string contract_kind{};
};

struct OwnedFill {
    FillRow fill{};
    InstrumentScale scale{};
};

struct IdemRecord {
    std::string key{};
    std::string request_hash{};
    std::string state{};
    std::int32_t status{0};
    std::string body{};
    std::string at_ns{};
};

struct LedgerRecord {
    std::optional<OwnedOrder> order{};
    std::optional<OwnedParent> parent{};
    std::optional<OwnedFill> fill{};
    std::optional<IdemRecord> idem{};
};

class LedgerLog {
public:
    LedgerLog() = default;
    LedgerLog(const LedgerLog&) = delete;
    LedgerLog& operator=(const LedgerLog&) = delete;
    virtual ~LedgerLog() = default;

    // false when the record is not durable; a mutation must not go ahead on that
    [[nodiscard]] virtual bool append(const LedgerRecord& record) = 0;
    virtual void replay(const std::function<void(const LedgerRecord&)>& visit) = 0;
};

using WallClock = std::function<UnixNanos()>;

} // namespace raikiri::clientapi
