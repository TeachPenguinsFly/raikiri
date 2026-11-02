#pragma once

// capital, limits and states of the books: what people set, what the gate reads, what the log hears
// everything runs on the strategy loop, so nothing here locks

#include <raikiri/audit/log.hpp>
#include <raikiri/bookrisk/limits.hpp>
#include <raikiri/bookrisk/ports.hpp>
#include <raikiri/bookrisk/settings.hpp>
#include <raikiri/bookrisk/track.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/oms/risk.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace raikiri::bookrisk {

struct ChangeRequest {
    // the person; empty is refused
    std::string by{};
    std::string reason{};
    // says the person saw that this lets more through
    bool confirm{false};
};

enum class DeskError : std::uint8_t { Invalid, NeedsConfirm, NoSuchBook, NotDurable };

struct DeskRefusal {
    DeskError error{DeskError::Invalid};
    std::string detail{};
    // what would let more through, for NeedsConfirm
    std::vector<std::string> raises{};
};

struct LimitPatch {
    std::vector<std::pair<Param, Limit>> limits{};
    std::optional<LossAction> loss_action{};
};

struct LimitView {
    Limit set{};
    Resolved resolved{};
};

struct RowView {
    std::array<LimitView, k_param_count> limits{};
    LossAction loss_action{LossAction::Off};
    bool loss_action_set{false};
};

struct WindowView {
    FixedE9 change{FixedE9::from_raw(0)};
    std::uint64_t covered_s{0};
    // a level is set and this change is past it
    bool past_level{false};
};

struct PnlView {
    FixedE9 realized{FixedE9::from_raw(0)};
    FixedE9 unrealized{FixedE9::from_raw(0)};
    FixedE9 total{FixedE9::from_raw(0)};
    WindowView m1{};
    WindowView m5{};
    WindowView h1{};
    FixedE9 day{FixedE9::from_raw(0)};
    bool past_daily{false};
    std::uint32_t positions{0};
    std::uint32_t unmarked{0};
    std::uint32_t unvalued{0};
};

struct BookView {
    BookId id{0};
    std::string name{};
    StateRecord own{};
    // the stricter of the book's own state and the desk's
    oms::BookState state{oms::BookState::Active};
    std::size_t open_orders{0};
    RowView row{};
    PnlView pnl{};
};

struct DeskView {
    FixedE9 capital{FixedE9::undef()};
    std::string capital_by{};
    UnixNanos capital_at{};
    BalanceReading reported{};
    std::string capital_warning{};
    StateRecord desk{};
    RowView defaults{};
    std::vector<BookView> books{};
};

class RiskDesk {
public:
    RiskDesk(BookKernel& kernel, SettingsStore& store, audit::Sink& audit, Notifier& notifier, const Clock& clock);

    // reads the store; refuses a store it cannot read, since a lost halt must not start trading
    [[nodiscard]] std::expected<void, std::string> load();

    // the first order of a book builds its slot; after that these neither allocate nor block
    [[nodiscard]] const oms::RiskGate& gate(BookId book);
    void admit(BookId book, const OrderCmd& cmd, oms::Exposure& exposure);
    void sent(BookId book, const OrderCmd& cmd, const oms::Exposure& exposure);
    void filled(BookId book, InstrumentId instrument);
    // stops a book the gate refused for re-entering; the order that tripped it was not sent
    void refused(BookId book, const oms::RiskRefusal& refusal);

    // at the loop's pace; does its work once a second
    void tick();

    [[nodiscard]] std::expected<void, DeskRefusal> set_capital(FixedE9 capital, const ChangeRequest& request);
    [[nodiscard]] std::expected<void, DeskRefusal> set_defaults(const LimitPatch& patch, const ChangeRequest& request);
    [[nodiscard]] std::expected<void, DeskRefusal>
    set_limits(BookId book, const LimitPatch& patch, const ChangeRequest& request);
    // active or reducing; a halt goes through kill, so it always pulls the orders
    [[nodiscard]] std::expected<void, DeskRefusal>
    set_state(BookId book, oms::BookState state, const ChangeRequest& request);
    // a halt holds even if it cannot be saved, which is then said in the refusal; nullopt is every book
    [[nodiscard]] std::expected<std::size_t, DeskRefusal> kill(std::optional<BookId> book,
                                                               const ChangeRequest& request);
    [[nodiscard]] std::expected<void, DeskRefusal> resume_desk(const ChangeRequest& request);

    // first-time capital for a demo or a test, from the command line; never replaces a person's figure
    void seed_capital(FixedE9 capital, std::string_view by);

    // an automatic check found the book past a limit: only shrinking orders pass until a person resumes it
    void reduce_book(BookId book, std::string_view reason);

    [[nodiscard]] DeskView view();
    [[nodiscard]] std::optional<BookView> book_view(std::string_view id_or_name);

private:
    struct Slot {
        BookId id{0};
        // the risk file may key a book by a name it only has once it is registered
        std::string name{};
        oms::RiskGate gate{};
        ResolvedRow resolved{};
        BookTrack track{};
        LossSpeed speed{};
        PnlReading last{};
        bool past_1m{false};
        bool past_5m{false};
        bool past_1h{false};
        bool past_day{false};
    };

    [[nodiscard]] Slot& slot_of(BookId book);
    void rebuild(Slot& slot);
    void rebuild_all();

    [[nodiscard]] BookSettings& settings_of(BookId book) { return doc_.books[book]; }

    void sync_books();

    void sample(Slot& slot, UnixNanos now);
    void watch_level(Slot& slot, Param level, std::string_view label, FixedE9 change, bool& past, UnixNanos now);
    void watch_day(Slot& slot, FixedE9 day_pnl, UnixNanos now);
    void steady_line(const Slot& slot, UnixNanos now);
    void refresh_balances(UnixNanos now);

    struct Halted {
        std::size_t cancelled{0};
        std::string not_saved{};
    };

    Halted halt(Slot& slot, std::string_view by, std::string_view reason, std::string_view action);
    [[nodiscard]] std::size_t pull(std::optional<BookId> book, UnixNanos now);
    // halted when the settings were read; their orders may still rest at the venue
    std::vector<std::optional<BookId>> halted_at_load_;
    void retry_pulls(UnixNanos now);
    void pull_halted_after_start(UnixNanos now);

    [[nodiscard]] std::expected<void, DeskRefusal> commit(DeskSettings next);
    [[nodiscard]] std::vector<std::string> raises(const DeskSettings& before, const DeskSettings& after) const;
    [[nodiscard]] std::expected<void, DeskRefusal> check_request(const ChangeRequest& request) const;

    [[nodiscard]] RowView row_view(const LimitRow& set, const ResolvedRow& resolved, bool action_set) const;
    [[nodiscard]] BookView book_view_of(const Slot& slot) const;
    [[nodiscard]] std::string warning_text() const;

    void record(std::string_view by, std::string_view action, std::string_view subject, std::string_view detail);
    void
    notify(NoticeLevel level, std::string_view kind, std::string_view subject, std::string_view text, UnixNanos at);

    BookKernel* kernel_;
    SettingsStore* store_;
    audit::Sink* audit_;
    Notifier* notifier_;
    const Clock* clock_;
    DeskSettings doc_{};
    std::unordered_map<BookId, std::unique_ptr<Slot>> slots_{};
    BalanceReading reported_{};
    bool warned_{false};
    std::uint64_t next_sample_ns_{0};
    std::uint64_t next_steady_ns_{0};
    std::uint64_t next_balances_ns_{0};
    // books whose orders are still being pulled, until when; a cancel that found its ring full is sent again
    std::map<std::optional<BookId>, std::uint64_t> pulling_{};
    std::uint64_t next_pull_ns_{0};
};

} // namespace raikiri::bookrisk
