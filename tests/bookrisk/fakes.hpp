#pragma once

#include <raikiri/bookrisk/desk.hpp>

#include <map>
#include <string>
#include <vector>

namespace raikiri::bookrisk::test {

inline FixedE9 d(std::string_view text) {
    return *parse_decimal_e9(text);
}

inline OrderCmd buy(std::uint64_t at_ns, std::int64_t qty = 1, Side side = Side::Buy) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = side;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{2});
    cmd.client_order_id = ClientOrderId::make(7, 1);
    cmd.price = PriceTicks::from_raw(100);
    cmd.qty = QtyLots::from_raw(qty);
    cmd.ts_created = {at_ns};
    return cmd;
}

class FakeKernel final : public BookKernel {
public:
    oms::RiskConfig file{};
    std::map<BookId, oms::RiskConfig> per_book{};
    std::vector<BookId> books{7};
    std::map<BookId, std::size_t> open{};
    std::map<BookId, PnlReading> pnls{};
    BalanceReading held{};
    std::vector<std::optional<BookId>> cancels{};

    [[nodiscard]] oms::RiskConfig base_config(BookId book) const override {
        const auto it = per_book.find(book);
        return it == per_book.end() ? file : it->second;
    }

    [[nodiscard]] oms::RiskConfig base_defaults() const override { return file; }

    [[nodiscard]] std::string name_of(BookId book) const override { return "book-" + std::to_string(book); }

    void known_books(std::vector<BookId>& out) const override { out.insert(out.end(), books.begin(), books.end()); }

    std::size_t cancel_open(std::optional<BookId> book) override {
        cancels.push_back(book);
        return open_orders(book);
    }

    [[nodiscard]] std::size_t open_orders(std::optional<BookId> book) const override {
        std::size_t out = 0;
        for (const auto& [id, count] : open) {
            out += !book.has_value() || id == *book ? count : 0;
        }
        return out;
    }

    [[nodiscard]] PnlReading pnl(BookId book) const override {
        const auto it = pnls.find(book);
        return it == pnls.end() ? PnlReading{} : it->second;
    }

    [[nodiscard]] BalanceReading balances() const override { return held; }
};

class MemoryStore final : public SettingsStore {
public:
    std::optional<DeskSettings> saved{};
    bool fail{false};
    std::uint32_t saves{0};

    [[nodiscard]] std::expected<std::optional<DeskSettings>, std::string> load() override { return saved; }

    [[nodiscard]] std::expected<void, std::string> save(const DeskSettings& settings) override {
        if (fail) {
            return std::unexpected{std::string{"disk full"}};
        }
        saved = settings;
        ++saves;
        return {};
    }
};

struct AuditRow {
    std::string actor;
    std::string action;
    std::string subject;
    std::string detail;
};

class MemoryAudit final : public audit::Sink {
public:
    std::vector<AuditRow> rows{};

    void append(const audit::Entry& entry) override {
        rows.push_back({std::string{entry.actor},
                        std::string{entry.action},
                        std::string{entry.subject},
                        std::string{entry.detail_json}});
    }
};

struct NoticeRow {
    NoticeLevel level{};
    std::string kind;
    std::string subject;
    std::string text;
};

class MemoryNotifier final : public Notifier {
public:
    std::vector<NoticeRow> rows{};

    void notify(const Notice& notice) override {
        rows.push_back({notice.level, std::string{notice.kind}, std::string{notice.subject}, std::string{notice.text}});
    }

    [[nodiscard]] std::size_t count(std::string_view kind) const {
        std::size_t out = 0;
        for (const auto& row : rows) {
            out += row.kind == kind ? 1U : 0U;
        }
        return out;
    }
};

} // namespace raikiri::bookrisk::test
