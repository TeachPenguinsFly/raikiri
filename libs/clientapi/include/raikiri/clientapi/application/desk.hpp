#pragma once

// the client's use cases; each takes the calling key and sees only what that key may see

#include <raikiri/clientapi/application/book.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/application/ports.hpp>
#include <raikiri/clientapi/application/refusal.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::clientapi {

// listings do not change owner, so a short cache saves a loopback call per order
class InstrumentDirectory {
public:
    InstrumentDirectory(Backend& backend, WallClock wall) : backend_{&backend}, wall_{std::move(wall)} {}

    [[nodiscard]] BackendResult<InstrumentRow> lookup(const std::string& id);

private:
    struct Cached {
        InstrumentRow row{};
        std::uint64_t at_ns{0};
    };

    Backend* backend_;
    WallClock wall_;
    std::mutex mutex_;
    std::map<std::string, Cached> cache_;
};

[[nodiscard]] Refusal refusal_of(const BackendError& error, std::string_view code = "backend-refused");

// what a client sees for a denied order's reject code; empty for a venue's own code
[[nodiscard]] std::optional<OrderRefusal> order_refusal_of(std::int64_t reject_code);

[[nodiscard]] std::string book_name_of(const ApiKey& key);

struct PlaceResult {
    std::string client_order_id{};
    std::string status{"queued"};
    std::string client_id{};
    std::optional<std::string> replaces{};
};

struct CancelResult {
    std::string client_order_id{};
    std::string status{"cancel_queued"};
};

struct CancelAllResult {
    std::vector<std::string> canceled{};
    std::vector<std::string> failed{};
};

struct AmendPolicy {
    std::chrono::milliseconds wait{2'000};
    std::chrono::milliseconds poll{50};
};

class ClientDesk {
public:
    using Sleep = std::function<void(std::chrono::milliseconds)>;

    ClientDesk(Backend& backend,
               ClientBook& book,
               InstrumentDirectory& instruments,
               WallClock wall,
               Sleep sleep,
               AmendPolicy amend = {})
        : backend_{&backend}, book_{&book}, instruments_{&instruments}, wall_{std::move(wall)},
          sleep_{std::move(sleep)}, amend_{amend} {}

    [[nodiscard]] Outcome<PlaceResult>
    place(const ApiKey& key, const NewOrder& order, std::string_view idempotency_key);
    [[nodiscard]] Outcome<CancelResult> cancel(const ApiKey& key, const std::string& id);
    [[nodiscard]] Outcome<PlaceResult>
    amend(const ApiKey& key, const std::string& id, const AmendOrder& change, std::string_view idempotency_key);
    [[nodiscard]] Outcome<CancelAllResult>
    cancel_all(const ApiKey& key, std::string_view account, std::string_view instrument_id);

    [[nodiscard]] Outcome<OrderRow> order(const ApiKey& key, const std::string& id);
    [[nodiscard]] Outcome<std::vector<OrderEventRow>> order_events(const ApiKey& key, const std::string& id);
    [[nodiscard]] Outcome<Page<OrderRow>> orders(const ApiKey& key, const OrderQuery& query);

    [[nodiscard]] Outcome<ParentRow> start_algo(const ApiKey& key, const NewParent& parent);
    [[nodiscard]] Outcome<ParentRow> algo(const ApiKey& key, const std::string& id);
    [[nodiscard]] Outcome<std::vector<ParentRow>> algos(const ApiKey& key);
    [[nodiscard]] Outcome<ParentRow> algo_action(const ApiKey& key, const std::string& id, std::string_view action);

    [[nodiscard]] Page<FillRow>
    fills(const ApiKey& key, const FillFilter& filter, std::string_view cursor, std::size_t limit) const;
    [[nodiscard]] std::vector<PositionRow>
    positions(const ApiKey& key, std::string_view account, std::string_view instrument_id) const;
    [[nodiscard]] Outcome<std::vector<AccountBalances>> balances(const ApiKey& key, std::string_view account);
    [[nodiscard]] Outcome<std::vector<AccountRow>> accounts(const ApiKey& key);

    [[nodiscard]] Outcome<InstrumentRow> instrument(const ApiKey& key, const std::string& id);
    [[nodiscard]] Outcome<Page<InstrumentRow>> instruments(
        const ApiKey& key, std::string_view account, std::string_view kind, std::string_view cursor, std::size_t limit);
    [[nodiscard]] Outcome<std::vector<MarketRow>> markets(const ApiKey& key, std::string_view query, std::size_t limit);

    // refused unless the key may see every symbol or the underlying's own
    [[nodiscard]] Outcome<std::string> options(const ApiKey& key, std::string_view underlying, std::string_view target);

private:
    // once per client, then remembered; admind answers a repeat as already open
    [[nodiscard]] Outcome<void> ensure_book(const ApiKey& key);
    [[nodiscard]] Outcome<OwnedOrder> owned_order(const ApiKey& key, const std::string& id) const;
    [[nodiscard]] Outcome<OwnedParent> owned_parent(const ApiKey& key, const std::string& id) const;
    [[nodiscard]] Outcome<InstrumentRow> tradable(const ApiKey& key, const NewOrder& order);
    [[nodiscard]] Outcome<PlaceResult> submit(const ApiKey& key,
                                              const NewOrder& order,
                                              const InstrumentRow& listing,
                                              std::string_view upstream_key,
                                              std::optional<std::string> replaces);
    [[nodiscard]] Outcome<ParentRow> checked_parent(const OwnedParent& owned);
    [[nodiscard]] OrderRow decorated(OrderRow row, const OwnedOrder& owned) const;

    Backend* backend_;
    ClientBook* book_;
    InstrumentDirectory* instruments_;
    WallClock wall_;
    Sleep sleep_;
    AmendPolicy amend_;
    std::mutex books_mutex_;
    std::set<std::string> books_open_;
};

} // namespace raikiri::clientapi
