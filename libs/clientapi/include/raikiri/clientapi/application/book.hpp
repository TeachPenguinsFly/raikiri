#pragma once

// who owns what: the backend's desk mints every client's order under one strategy id, so ownership lives here

#include <raikiri/clientapi/application/ports.hpp>
#include <raikiri/portfolio/position.hpp>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::clientapi {

struct FillFilter {
    std::string_view account{};
    std::string_view instrument_id{};
    std::uint64_t since_ns{0};
};

// what the live event feed has said about an order; postgres says the same later
struct LiveOrder {
    std::optional<std::string> closed_as{};
    std::optional<std::string> leaves_after_fill{};
};

class ClientBook {
public:
    ClientBook(LedgerLog& log, std::uint64_t idempotency_ttl_ns) noexcept
        : log_{&log}, idempotency_{idempotency_ttl_ns} {}

    // replays the log; call once, before anything else runs
    void load(std::uint64_t now_ns);

    [[nodiscard]] bool record_order(const OwnedOrder& order);
    [[nodiscard]] bool record_parent(const OwnedParent& parent);
    void close_parent(const std::string& id);

    [[nodiscard]] std::optional<OwnedOrder> find_order(const std::string& id) const;
    void note_event(const OrderEventRow& event);
    [[nodiscard]] std::optional<LiveOrder> live_order(const std::string& id) const;
    [[nodiscard]] std::optional<OwnedParent> find_parent(const std::string& id) const;
    [[nodiscard]] std::vector<OwnedParent> open_parents() const;
    [[nodiscard]] std::vector<OwnedParent> parents_of(const std::string& client_id) const;

    // false when venue and exec id were booked before; the second answer is that client's position after it
    [[nodiscard]] std::optional<PositionRow> record_fill(const OwnedFill& fill);
    [[nodiscard]] Page<FillRow>
    fills_of(const std::string& client_id, const FillFilter& filter, std::string_view cursor, std::size_t limit) const;
    [[nodiscard]] std::vector<PositionRow> positions_of(const std::string& client_id) const;
    [[nodiscard]] std::uint64_t last_fill_ns() const;

    [[nodiscard]] IdemBegin
    idem_begin(const std::string& key, std::string_view hash, std::uint64_t now_ns, bool& durable);
    void idem_finish(const std::string& key, const StoredReply& reply);
    void idem_unknown(const std::string& key);
    void idem_release(const std::string& key);

private:
    struct Held {
        portfolio::Position position{};
        InstrumentBase base{};
        std::string account{};
        std::string symbol{};
        std::string instrument_id{};
    };

    void apply_order(const OwnedOrder& order);
    void apply_parent(const OwnedParent& parent);
    [[nodiscard]] bool apply_fill(const OwnedFill& fill);
    void apply_idem(const IdemRecord& record, std::uint64_t now_ns);
    [[nodiscard]] PositionRow row_of(const std::string& client_id, const Held& held) const;

    LedgerLog* log_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, OwnedOrder> orders_;
    std::unordered_map<std::string, LiveOrder> live_;
    std::map<std::string, OwnedParent> parents_;
    std::set<std::pair<std::string, std::string>> booked_;
    std::unordered_map<std::string, std::vector<FillRow>> fills_;
    std::map<std::pair<std::string, std::string>, Held> held_;
    std::uint64_t last_fill_ns_{0};
    IdempotencyTable idempotency_;
};

} // namespace raikiri::clientapi
