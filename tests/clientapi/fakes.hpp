#pragma once

#include <raikiri/clientapi/application/ports.hpp>
#include <raikiri/clientapi/domain/key.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace raikiri::clientapi::test {

inline constexpr std::string_view k_secret = "0123456789abcdef0123456789abcdef";

inline std::string sign(std::string_view message, std::string_view secret = k_secret) {
    std::array<char, net::k_hex_bytes> out{};
    return std::string{net::hmac_sha256_hex(secret, message, out)};
}

class FakeKeys final : public KeyDirectory {
public:
    std::shared_ptr<const ApiKey> add(ApiKey key) {
        auto shared = std::make_shared<const ApiKey>(std::move(key));
        keys[shared->id] = shared;
        return shared;
    }

    [[nodiscard]] std::shared_ptr<const ApiKey> find(std::string_view id) const override {
        const auto found = keys.find(std::string{id});
        return found == keys.end() ? nullptr : found->second;
    }

    [[nodiscard]] bool verify(const ApiKey&, std::string_view message, std::string_view signature) const override {
        return sign(message) == signature;
    }

    std::map<std::string, std::shared_ptr<const ApiKey>> keys;
};

class MemoryLedger final : public LedgerLog {
public:
    bool append(const LedgerRecord& record) override {
        if (fail) {
            return false;
        }
        records.push_back(record);
        return true;
    }

    void replay(const std::function<void(const LedgerRecord&)>& visit) override {
        for (const auto& record : records) {
            visit(record);
        }
    }

    std::vector<LedgerRecord> records;
    bool fail{false};
};

class MemoryAudit final : public AuditSink {
public:
    struct Line {
        std::string actor, action, subject, detail;
    };

    void append(const AuditEntry& entry) override {
        lines.push_back({std::string{entry.actor},
                         std::string{entry.action},
                         std::string{entry.subject},
                         std::string{entry.detail_json}});
    }

    [[nodiscard]] std::size_t count(std::string_view action) const {
        std::size_t n = 0;
        for (const auto& line : lines) {
            if (line.action == action) {
                ++n;
            }
        }
        return n;
    }

    std::vector<Line> lines;
};

inline InstrumentRow listing(std::string id, std::string account, std::string symbol) {
    return {.instrument_id = std::move(id),
            .account = std::move(account),
            .venue = "binance",
            .market = "perp",
            .exchange_symbol = symbol,
            .local_symbol = symbol,
            .tick_size = "0.1",
            .lot_size = "0.001",
            .min_notional = "5",
            .contract_value = "1",
            .contract_kind = "linear",
            .asset_class = "crypto"};
}

class FakeBackend final : public Backend {
public:
    BackendResult<void> open_book(std::string_view name) override {
        books_opened.emplace_back(name);
        if (book_failure.has_value()) {
            return std::unexpected(*book_failure);
        }
        return {};
    }

    BackendResult<std::string> place_order(const NewOrder& order, std::string_view key) override {
        placed.push_back(order);
        keys_seen.emplace_back(key);
        if (place_failure.has_value()) {
            return std::unexpected(*place_failure);
        }
        return std::to_string(++next_order);
    }

    BackendResult<void> cancel_order(std::string_view id) override {
        canceled.emplace_back(id);
        if (const auto found = rows.find(std::string{id}); found != rows.end() && close_on_cancel) {
            found->second.status = "canceled";
        }
        return {};
    }

    BackendResult<OrderRow> order(std::string_view id) override {
        const auto found = rows.find(std::string{id});
        if (found == rows.end()) {
            return std::unexpected(BackendError{.status = 404, .title = "no such order"});
        }
        return found->second;
    }

    BackendResult<std::vector<OrderEventRow>> order_events(std::string_view) override { return events; }

    BackendResult<Page<OrderRow>> orders(const OrderQuery& query) override {
        ++order_scans;
        last_order_book = query.book;
        Page<OrderRow> page;
        auto at = query.cursor.empty() ? std::size_t{0} : static_cast<std::size_t>(std::stoul(query.cursor));
        while (at < order_list.size() && page.items.size() < query.limit) {
            page.items.push_back(order_list[at++]);
        }
        if (at < order_list.size()) {
            page.next_cursor = std::to_string(at);
        }
        return page;
    }

    BackendResult<Page<FillRow>> fills(std::string_view, std::string_view) override { return fill_page; }

    BackendResult<InstrumentRow> instrument(std::string_view id) override {
        const auto found = listings.find(std::string{id});
        if (found == listings.end()) {
            return std::unexpected(BackendError{.status = 404, .title = "no such instrument"});
        }
        return found->second;
    }

    BackendResult<Page<InstrumentRow>>
    instruments(std::string_view, std::string_view, std::string_view, std::size_t) override {
        Page<InstrumentRow> page;
        for (const auto& [id, row] : listings) {
            page.items.push_back(row);
        }
        return page;
    }

    BackendResult<std::vector<MarketRow>> markets(std::string_view, std::size_t) override {
        return std::vector<MarketRow>{};
    }

    BackendResult<std::vector<AccountRow>> accounts() override {
        return std::vector<AccountRow>{{.account = "binance", .alive = true}, {.account = "okx", .alive = true}};
    }

    BackendResult<std::vector<AccountBalances>> balances() override { return balance_rows; }

    BackendResult<std::string> start_parent(const NewParent& parent) override {
        started.push_back(parent);
        const auto id = std::to_string(++next_parent);
        parents[id] = ParentRow{
            .id = id, .symbol = parent.symbol, .side = parent.side, .status = "working", .created = "1000"};
        return id;
    }

    BackendResult<ParentRow> parent(std::string_view id) override {
        const auto found = parents.find(std::string{id});
        if (found == parents.end()) {
            return std::unexpected(BackendError{.status = 404, .title = "no such parent"});
        }
        return found->second;
    }

    BackendResult<ParentRow> parent_action(std::string_view id, std::string_view action) override {
        actions.push_back(std::string{id} + ":" + std::string{action});
        return parent(id);
    }

    BackendResult<std::string> read_raw(std::string_view target) override {
        return "{\"target\":\"" + std::string{target} + "\"}";
    }

    BackendResult<std::string> stream_token() override { return "token"; }

    std::vector<std::string> books_opened;
    std::optional<BackendError> book_failure;
    std::string last_order_book;
    std::vector<NewOrder> placed;
    std::vector<std::string> keys_seen;
    std::vector<std::string> canceled;
    std::vector<std::string> actions;
    std::vector<NewParent> started;
    std::map<std::string, OrderRow> rows;
    std::vector<OrderRow> order_list;
    std::vector<OrderEventRow> events;
    Page<FillRow> fill_page;
    std::map<std::string, InstrumentRow> listings;
    std::vector<AccountBalances> balance_rows;
    std::map<std::string, ParentRow> parents;
    std::optional<BackendError> place_failure;
    bool close_on_cancel{true};
    std::uint64_t next_order{100};
    std::uint64_t next_parent{0};
    int order_scans{0};
};

inline ApiKey full_key(std::string id = "ak1", std::string client = "acme") {
    return {.id = std::move(id),
            .client_id = std::move(client),
            .permissions = 7,
            .accounts = {"*"},
            .symbols = {"*"},
            .rate = {.requests_per_second = 1000, .burst = 1000, .mutations_per_second = 1000, .mutation_burst = 1000}};
}

} // namespace raikiri::clientapi::test
