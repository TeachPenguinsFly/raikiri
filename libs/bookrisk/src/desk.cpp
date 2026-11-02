#include <raikiri/bookrisk/desk.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <ranges>

namespace raikiri::bookrisk {
namespace internal {

constexpr std::uint64_t k_s = 1'000'000'000;
constexpr std::uint64_t k_sample_ns = k_s;
constexpr std::uint64_t k_steady_ns = 60 * k_s;
constexpr std::uint64_t k_balances_ns = 30 * k_s;
constexpr std::uint64_t k_pull_every_ns = 5 * k_s;
constexpr std::uint64_t k_pull_for_ns = 60 * k_s;
constexpr std::uint64_t k_day_ns = 86'400 * k_s;
constexpr std::uint64_t k_m1 = 60 * k_s;
constexpr std::uint64_t k_m5 = 300 * k_s;
constexpr std::uint64_t k_h1 = 3'600 * k_s;

[[nodiscard]] std::string money(FixedE9 value) {
    if (value.is_undef()) {
        return "off";
    }
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

[[nodiscard]] std::string quoted(std::string_view text) {
    std::string out{"\""};
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += fmt::format("\\u{:04x}", static_cast<int>(c));
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string limit_json(const Limit& limit) {
    if (limit.mode == LimitMode::Inherit || limit.mode == LimitMode::Off) {
        return quoted(to_string(limit.mode));
    }
    return fmt::format(R"({{"mode":"{}","value":"{}"}})", to_string(limit.mode), money(limit.value));
}

// only what moved, under its name
[[nodiscard]] std::string moved_json(const LimitRow& before, const LimitRow& after) {
    std::string old_side;
    std::string new_side;
    for (const auto& info : k_params) {
        if (before[info.param] == after[info.param]) {
            continue;
        }
        const auto* comma = old_side.empty() ? "" : ",";
        old_side += fmt::format(R"({}"{}":{})", comma, info.name, limit_json(before[info.param]));
        new_side += fmt::format(R"({}"{}":{})", comma, info.name, limit_json(after[info.param]));
    }
    if (before.loss_action != after.loss_action) {
        const auto named = [](const std::optional<LossAction>& action) {
            return action.transform([](LossAction set) { return to_string(set); }).value_or("inherit");
        };
        const auto* comma = old_side.empty() ? "" : ",";
        old_side += fmt::format(R"({}"daily_loss_action":"{}")", comma, named(before.loss_action));
        new_side += fmt::format(R"({}"daily_loss_action":"{}")", comma, named(after.loss_action));
    }
    return fmt::format(R"("old":{{{}}},"new":{{{}}})", old_side, new_side);
}

[[nodiscard]] std::string book_subject(BookId book) {
    return fmt::format("book:{}", book);
}

[[nodiscard]] FixedE9 add(FixedE9 a, FixedE9 b) noexcept {
    return FixedE9::from_raw(a.raw + b.raw);
}

} // namespace internal

RiskDesk::RiskDesk(BookKernel& kernel, SettingsStore& store, audit::Sink& audit, Notifier& notifier, const Clock& clock)
    : kernel_{&kernel}, store_{&store}, audit_{&audit}, notifier_{&notifier}, clock_{&clock} {}

std::expected<void, std::string> RiskDesk::load() {
    auto loaded = store_->load();
    if (!loaded.has_value()) {
        return std::unexpected{std::move(loaded.error())};
    }
    if (auto& saved = *loaded; saved.has_value()) {
        doc_ = std::move(*saved);
    }
    if (doc_.desk.state == oms::BookState::Halted) {
        halted_at_load_.emplace_back(std::nullopt);
    }
    for (const auto& [id, settings] : doc_.books) {
        if (settings.state.state == oms::BookState::Halted) {
            halted_at_load_.emplace_back(id);
        }
    }
    return {};
}

RiskDesk::Slot& RiskDesk::slot_of(BookId book) {
    if (const auto it = slots_.find(book); it != slots_.end()) {
        return *it->second;
    }
    auto& slot = *slots_.emplace(book, std::make_unique<Slot>()).first->second;
    slot.id = book;
    rebuild(slot);
    return slot;
}

void RiskDesk::rebuild(Slot& slot) {
    slot.name = kernel_->name_of(slot.id);
    const auto base = kernel_->base_config(slot.id);
    slot.resolved = resolve(base, doc_.defaults, settings_of(slot.id).limits, doc_.capital);
    slot.gate = oms::RiskGate{to_config(base, slot.resolved)};
}

void RiskDesk::rebuild_all() {
    for (auto& [id, slot] : slots_) {
        rebuild(*slot);
    }
}

const oms::RiskGate& RiskDesk::gate(BookId book) {
    return slot_of(book).gate;
}

void RiskDesk::admit(BookId book, const OrderCmd& cmd, oms::Exposure& exposure) {
    const auto& slot = slot_of(book);
    exposure.capital_set = !doc_.capital.is_undef();
    exposure.book_state = oms::stricter(doc_.desk.state, doc_.books.find(book)->second.state.state);
    slot.track.fill(cmd, slot.gate.config(), exposure);
}

void RiskDesk::sent(BookId book, const OrderCmd& cmd, const oms::Exposure& exposure) {
    slot_of(book).track.on_sent(cmd, exposure);
}

void RiskDesk::filled(BookId book, InstrumentId instrument) {
    slot_of(book).track.on_fill(instrument);
}

void RiskDesk::refused(BookId book, const oms::RiskRefusal& refusal) {
    if (refusal.why != oms::RiskReject::RepeatedExecution) {
        return;
    }
    (void)halt(slot_of(book), "strategyd", oms::describe(refusal), "risk.book.tripped");
}

RiskDesk::Halted RiskDesk::halt(Slot& slot, std::string_view by, std::string_view reason, std::string_view action) {
    const auto now = clock_->wall();
    auto& own = settings_of(slot.id).state;
    const auto old = own.state;
    own = {.state = oms::BookState::Halted, .reason = std::string{reason}, .by = std::string{by}, .since = now};
    const auto saved = store_->save(doc_);
    const auto cancelled = pull(slot.id, now);
    const auto subject = internal::book_subject(slot.id);
    record(by,
           action,
           subject,
           fmt::format(R"({{"old":"{}","new":"halted","reason":{},"cancelled":{},"durable":{}}})",
                       oms::to_string(old),
                       internal::quoted(reason),
                       cancelled,
                       saved.has_value()));
    notify(NoticeLevel::Critical,
           "book_halted",
           subject,
           fmt::format("{} halted by {}: {}; {} cancels sent{}",
                       kernel_->name_of(slot.id),
                       by,
                       reason,
                       cancelled,
                       saved.has_value() ? "" : "; the halt could not be saved and will not survive a restart"),
           now);
    return {.cancelled = cancelled, .not_saved = saved.has_value() ? std::string{} : saved.error()};
}

std::size_t RiskDesk::pull(std::optional<BookId> book, UnixNanos now) {
    pulling_[book] = now.ns + internal::k_pull_for_ns;
    next_pull_ns_ = now.ns + internal::k_pull_every_ns;
    return kernel_->cancel_open(book);
}

void RiskDesk::retry_pulls(UnixNanos now) {
    if (pulling_.empty() || now.ns < next_pull_ns_) {
        return;
    }
    next_pull_ns_ = now.ns + internal::k_pull_every_ns;
    std::erase_if(pulling_, [&](const auto& entry) {
        const auto& [book, until] = entry;
        if (now.ns >= until || kernel_->open_orders(book) == 0) {
            return true;
        }
        (void)kernel_->cancel_open(book);
        return false;
    });
}

void RiskDesk::sync_books() {
    std::vector<BookId> known;
    kernel_->known_books(known);
    for (const auto& [id, settings] : doc_.books) {
        known.push_back(id);
    }
    for (const auto id : known) {
        (void)slot_of(id);
    }
    for (auto& [id, slot] : slots_) {
        if (slot->name != kernel_->name_of(id)) {
            rebuild(*slot);
        }
    }
}

void RiskDesk::tick() {
    const auto now = clock_->wall();
    if (now.ns < next_sample_ns_) {
        return;
    }
    next_sample_ns_ = now.ns + internal::k_sample_ns;
    sync_books();
    for (auto& [id, slot] : slots_) {
        sample(*slot, now);
    }
    if (now.ns >= next_steady_ns_) {
        next_steady_ns_ = now.ns + internal::k_steady_ns;
        for (const auto& [id, slot] : slots_) {
            steady_line(*slot, now);
        }
    }
    if (now.ns >= next_balances_ns_) {
        refresh_balances(now);
    }
    pull_halted_after_start(now);
    retry_pulls(now);
}

void RiskDesk::pull_halted_after_start(UnixNanos now) {
    for (const auto& book : halted_at_load_) {
        (void)pull(book, now);
    }
    halted_at_load_.clear();
}

void RiskDesk::reduce_book(BookId book, std::string_view reason) {
    const auto now = clock_->wall();
    auto& own = settings_of(book).state;
    if (own.state != oms::BookState::Active) {
        return;
    }
    own = {.state = oms::BookState::Reducing, .reason = std::string{reason}, .by = "strategyd", .since = now};
    const auto saved = store_->save(doc_);
    const auto subject = internal::book_subject(book);
    record("strategyd",
           "risk.book.tripped",
           subject,
           fmt::format(R"({{"old":"active","new":"reducing","reason":{},"durable":{}}})",
                       internal::quoted(reason),
                       saved.has_value()));
    notify(NoticeLevel::Critical, "book_reducing", subject, std::string{reason}, now);
    rebuild(slot_of(book));
}

void RiskDesk::sample(Slot& slot, UnixNanos now) {
    slot.last = kernel_->pnl(slot.id);
    if (slot.last.unmarked > 0) {
        return;
    }
    const auto total = internal::add(slot.last.realized, slot.last.unrealized);
    slot.speed.sample(now, total);
    auto& day = settings_of(slot.id).day;
    const auto today = static_cast<std::uint32_t>(now.ns / internal::k_day_ns);
    if (day.day != today) {
        day = {.day = today, .start_pnl = total};
        slot.past_day = false;
        // a day mark lost to a failed save only shortens the day's loss after a restart
        (void)store_->save(doc_);
    }
    watch_level(slot, Param::Loss1m, "1m", slot.speed.change(now, internal::k_m1).change, slot.past_1m, now);
    watch_level(slot, Param::Loss5m, "5m", slot.speed.change(now, internal::k_m5).change, slot.past_5m, now);
    watch_level(slot, Param::Loss1h, "1h", slot.speed.change(now, internal::k_h1).change, slot.past_1h, now);
    watch_day(slot, total - day.start_pnl, now);
}

void RiskDesk::watch_level(Slot& slot, Param level, std::string_view label, FixedE9 change, bool& past, UnixNanos now) {
    const auto limit = slot.resolved[level].effective;
    const bool beyond = !limit.is_undef() && change.raw <= -limit.raw;
    if (beyond && !past) {
        notify(NoticeLevel::Warning,
               "loss_speed",
               internal::book_subject(slot.id),
               fmt::format("{} moved {} over {}, level {}",
                           kernel_->name_of(slot.id),
                           internal::money(change),
                           label,
                           internal::money(limit)),
               now);
    }
    past = beyond;
}

void RiskDesk::watch_day(Slot& slot, FixedE9 day_pnl, UnixNanos now) {
    const auto limit = slot.resolved[Param::DailyLoss].effective;
    const bool beyond = !limit.is_undef() && day_pnl.raw <= -limit.raw;
    const bool crossed = beyond && !slot.past_day;
    slot.past_day = beyond;
    if (!crossed) {
        return;
    }
    const auto text = fmt::format(
        "{} lost {} today, level {}", kernel_->name_of(slot.id), internal::money(day_pnl), internal::money(limit));
    const auto subject = internal::book_subject(slot.id);
    notify(NoticeLevel::Warning, "daily_loss", subject, text, now);
    auto& own = settings_of(slot.id).state;
    if (slot.resolved.loss_action == LossAction::Halted && own.state != oms::BookState::Halted) {
        (void)halt(slot, "strategyd", text, "risk.book.loss_action");
    } else if (slot.resolved.loss_action == LossAction::Reducing && own.state == oms::BookState::Active) {
        own = {.state = oms::BookState::Reducing, .reason = text, .by = "strategyd", .since = now};
        const auto saved = store_->save(doc_);
        record("strategyd",
               "risk.book.loss_action",
               subject,
               fmt::format(R"({{"old":"active","new":"reducing","reason":{},"durable":{}}})",
                           internal::quoted(text),
                           saved.has_value()));
        notify(NoticeLevel::Critical, "book_reducing", subject, text, now);
    }
}

void RiskDesk::steady_line(const Slot& slot, UnixNanos now) {
    const auto total = internal::add(slot.last.realized, slot.last.unrealized);
    if (slot.last.positions == 0 && total.raw == 0) {
        return;
    }
    notify(NoticeLevel::Steady,
           "pnl",
           internal::book_subject(slot.id),
           fmt::format("{} pnl {} (realised {}, unrealised {}) 1m {} 5m {} 1h {}{}",
                       kernel_->name_of(slot.id),
                       internal::money(total),
                       internal::money(slot.last.realized),
                       internal::money(slot.last.unrealized),
                       internal::money(slot.speed.change(now, internal::k_m1).change),
                       internal::money(slot.speed.change(now, internal::k_m5).change),
                       internal::money(slot.speed.change(now, internal::k_h1).change),
                       slot.last.unmarked > 0 ? fmt::format(", {} positions unmarked", slot.last.unmarked) : ""),
           now);
}

void RiskDesk::refresh_balances(UnixNanos now) {
    next_balances_ns_ = now.ns + internal::k_balances_ns;
    reported_ = kernel_->balances();
    const auto text = warning_text();
    if (!text.empty() && !warned_) {
        notify(NoticeLevel::Warning, "capital_above_balances", "desk", text, now);
    }
    warned_ = !text.empty();
}

std::string RiskDesk::warning_text() const {
    if (doc_.capital.is_undef() || reported_.accounts == 0 || doc_.capital <= reported_.dollars) {
        return {};
    }
    return fmt::format("declared capital {} is above the {} balances report{}",
                       internal::money(doc_.capital),
                       internal::money(reported_.dollars),
                       reported_.unvalued > 0 ? fmt::format("; assets with no price here: {}", reported_.unvalued)
                                              : "");
}

std::expected<void, DeskRefusal> RiskDesk::check_request(const ChangeRequest& request) const {
    if (request.by.empty()) {
        return std::unexpected{DeskRefusal{.error = DeskError::Invalid, .detail = "say who is making this change: by"}};
    }
    return {};
}

std::vector<std::string> RiskDesk::raises(const DeskSettings& before, const DeskSettings& after) const {
    std::vector<std::string> out;
    const auto row_of = [](const DeskSettings& doc, BookId id) -> const LimitRow& {
        static const LimitRow k_none{};
        const auto it = doc.books.find(id);
        return it == doc.books.end() ? k_none : it->second.limits;
    };
    const auto base = kernel_->base_defaults();
    loosened(resolve(base, before.defaults, {}, before.capital), resolve(base, after.defaults, {}, after.capital), out);
    for (const auto& [id, slot] : slots_) {
        const auto file = kernel_->base_config(id);
        loosened(resolve(file, before.defaults, row_of(before, id), before.capital),
                 resolve(file, after.defaults, row_of(after, id), after.capital),
                 out);
    }
    for (const auto& [id, settings] : after.books) {
        if (!slots_.contains(id)) {
            const auto file = kernel_->base_config(id);
            loosened(resolve(file, before.defaults, row_of(before, id), before.capital),
                     resolve(file, after.defaults, row_of(after, id), after.capital),
                     out);
        }
    }
    return out;
}

std::expected<void, DeskRefusal> RiskDesk::commit(DeskSettings next) {
    if (const auto saved = store_->save(next); !saved.has_value()) {
        return std::unexpected{DeskRefusal{.error = DeskError::NotDurable, .detail = saved.error()}};
    }
    doc_ = std::move(next);
    rebuild_all();
    return {};
}

std::expected<void, DeskRefusal> RiskDesk::set_capital(FixedE9 capital, const ChangeRequest& request) {
    if (auto ok = check_request(request); !ok.has_value()) {
        return ok;
    }
    if (capital.is_undef() || capital.raw <= 0) {
        return std::unexpected{DeskRefusal{.error = DeskError::Invalid, .detail = "capital must be above 0"}};
    }
    auto next = doc_;
    next.capital = capital;
    next.capital_by = request.by;
    next.capital_at = clock_->wall();
    auto loose = raises(doc_, next);
    if (doc_.capital.is_undef() || capital > doc_.capital) {
        loose.insert(loose.begin(), "capital");
    }
    if (!loose.empty() && !request.confirm) {
        return std::unexpected{DeskRefusal{.error = DeskError::NeedsConfirm,
                                           .detail = "this lets more through; send confirm to go ahead",
                                           .raises = std::move(loose)}};
    }
    const auto old = doc_.capital;
    if (auto done = commit(std::move(next)); !done.has_value()) {
        return done;
    }
    record(request.by,
           "risk.capital.set",
           "desk",
           fmt::format(R"({{"old":{},"new":{},"reason":{},"confirm":{}}})",
                       old.is_undef() ? "null" : internal::quoted(internal::money(old)),
                       internal::quoted(internal::money(capital)),
                       internal::quoted(request.reason),
                       request.confirm));
    refresh_balances(clock_->wall());
    return {};
}

std::expected<void, DeskRefusal> RiskDesk::set_defaults(const LimitPatch& patch, const ChangeRequest& request) {
    if (auto ok = check_request(request); !ok.has_value()) {
        return ok;
    }
    auto next = doc_;
    for (const auto& [param, limit] : patch.limits) {
        if (auto why = check(param, limit); !why.empty()) {
            return std::unexpected{DeskRefusal{.error = DeskError::Invalid, .detail = std::move(why)}};
        }
        next.defaults[param] = limit;
    }
    if (patch.loss_action.has_value()) {
        next.defaults.loss_action = patch.loss_action;
    }
    if (auto loose = raises(doc_, next); !loose.empty() && !request.confirm) {
        return std::unexpected{DeskRefusal{.error = DeskError::NeedsConfirm,
                                           .detail = "this lets more through; send confirm to go ahead",
                                           .raises = std::move(loose)}};
    }
    const auto moved = internal::moved_json(doc_.defaults, next.defaults);
    if (auto done = commit(std::move(next)); !done.has_value()) {
        return done;
    }
    record(request.by,
           "risk.defaults.set",
           "defaults",
           fmt::format(R"({{{},"reason":{},"confirm":{}}})", moved, internal::quoted(request.reason), request.confirm));
    return {};
}

std::expected<void, DeskRefusal>
RiskDesk::set_limits(BookId book, const LimitPatch& patch, const ChangeRequest& request) {
    if (auto ok = check_request(request); !ok.has_value()) {
        return ok;
    }
    auto next = doc_;
    auto& row = next.books[book].limits;
    for (const auto& [param, limit] : patch.limits) {
        if (auto why = check(param, limit); !why.empty()) {
            return std::unexpected{DeskRefusal{.error = DeskError::Invalid, .detail = std::move(why)}};
        }
        row[param] = limit;
    }
    if (patch.loss_action.has_value()) {
        row.loss_action = patch.loss_action;
    }
    if (auto loose = raises(doc_, next); !loose.empty() && !request.confirm) {
        return std::unexpected{DeskRefusal{.error = DeskError::NeedsConfirm,
                                           .detail = "this lets more through; send confirm to go ahead",
                                           .raises = std::move(loose)}};
    }
    const auto before = settings_of(book).limits;
    const auto moved = internal::moved_json(before, row);
    if (auto done = commit(std::move(next)); !done.has_value()) {
        return done;
    }
    (void)slot_of(book);
    record(request.by,
           "risk.book.limits.set",
           internal::book_subject(book),
           fmt::format(R"({{{},"reason":{},"confirm":{}}})", moved, internal::quoted(request.reason), request.confirm));
    return {};
}

std::expected<void, DeskRefusal> RiskDesk::set_state(BookId book, oms::BookState state, const ChangeRequest& request) {
    if (auto ok = check_request(request); !ok.has_value()) {
        return ok;
    }
    if (state == oms::BookState::Halted) {
        return std::unexpected{DeskRefusal{.error = DeskError::Invalid,
                                           .detail = "a halt goes through the kill switch, which pulls the orders"}};
    }
    const auto old = settings_of(book).state.state;
    if (state == old) {
        return {};
    }
    if (state < old && !request.confirm) {
        return std::unexpected{DeskRefusal{.error = DeskError::NeedsConfirm,
                                           .detail = "this lets more through; send confirm to go ahead",
                                           .raises = {"state"}}};
    }
    auto next = doc_;
    next.books[book].state = {.state = state, .reason = request.reason, .by = request.by, .since = clock_->wall()};
    if (auto done = commit(std::move(next)); !done.has_value()) {
        return done;
    }
    if (state < old) {
        slot_of(book).track.forgive();
        pulling_.erase(std::optional<BookId>{book});
    }
    record(request.by,
           "risk.book.state.set",
           internal::book_subject(book),
           fmt::format(R"({{"old":"{}","new":"{}","reason":{},"confirm":{}}})",
                       oms::to_string(old),
                       oms::to_string(state),
                       internal::quoted(request.reason),
                       request.confirm));
    return {};
}

std::expected<std::size_t, DeskRefusal> RiskDesk::kill(std::optional<BookId> book, const ChangeRequest& request) {
    if (auto ok = check_request(request); !ok.has_value()) {
        return std::unexpected{ok.error()};
    }
    const auto reason = request.reason.empty() ? std::string{"kill switch"} : request.reason;
    if (book.has_value()) {
        auto done = halt(slot_of(*book), request.by, reason, "risk.book.kill");
        if (!done.not_saved.empty()) {
            return std::unexpected{DeskRefusal{.error = DeskError::NotDurable,
                                               .detail = "halted and pulling, but the halt could not be saved and "
                                                         "will not survive a restart: " +
                                                         done.not_saved}};
        }
        return done.cancelled;
    }
    const auto now = clock_->wall();
    const auto old = doc_.desk.state;
    doc_.desk = {.state = oms::BookState::Halted, .reason = reason, .by = request.by, .since = now};
    const auto saved = store_->save(doc_);
    const auto cancelled = pull(std::nullopt, now);
    record(request.by,
           "risk.desk.kill",
           "desk",
           fmt::format(R"({{"old":"{}","new":"halted","reason":{},"cancelled":{},"durable":{}}})",
                       oms::to_string(old),
                       internal::quoted(reason),
                       cancelled,
                       saved.has_value()));
    notify(NoticeLevel::Critical,
           "desk_halted",
           "desk",
           fmt::format("every book halted by {}: {}; {} cancels sent", request.by, reason, cancelled),
           now);
    if (!saved.has_value()) {
        return std::unexpected{DeskRefusal{
            .error = DeskError::NotDurable,
            .detail = "halted and pulling, but the halt could not be saved and will not survive a restart: " +
                      saved.error()}};
    }
    return cancelled;
}

std::expected<void, DeskRefusal> RiskDesk::resume_desk(const ChangeRequest& request) {
    if (auto ok = check_request(request); !ok.has_value()) {
        return ok;
    }
    if (doc_.desk.state == oms::BookState::Active) {
        return {};
    }
    if (!request.confirm) {
        return std::unexpected{DeskRefusal{.error = DeskError::NeedsConfirm,
                                           .detail = "this lets more through; send confirm to go ahead",
                                           .raises = {"desk_state"}}};
    }
    auto next = doc_;
    next.desk = {.state = oms::BookState::Active, .reason = request.reason, .by = request.by, .since = clock_->wall()};
    if (auto done = commit(std::move(next)); !done.has_value()) {
        return done;
    }
    pulling_.erase(std::optional<BookId>{});
    record(request.by,
           "risk.desk.resume",
           "desk",
           fmt::format(R"({{"old":"halted","new":"active","reason":{},"confirm":true}})",
                       internal::quoted(request.reason)));
    return {};
}

void RiskDesk::seed_capital(FixedE9 capital, std::string_view by) {
    if (!doc_.capital.is_undef()) {
        return;
    }
    doc_.capital = capital;
    doc_.capital_by = std::string{by};
    doc_.capital_at = clock_->wall();
    const auto saved = store_->save(doc_);
    rebuild_all();
    record(by,
           "risk.capital.seed",
           "desk",
           fmt::format(R"({{"old":null,"new":{},"durable":{}}})",
                       internal::quoted(internal::money(capital)),
                       saved.has_value()));
}

RowView RiskDesk::row_view(const LimitRow& set, const ResolvedRow& resolved, bool action_set) const {
    RowView out{.loss_action = resolved.loss_action, .loss_action_set = action_set};
    for (const auto& info : k_params) {
        out.limits[static_cast<std::size_t>(info.param)] = {.set = set[info.param], .resolved = resolved[info.param]};
    }
    return out;
}

BookView RiskDesk::book_view_of(const Slot& slot) const {
    const auto now = clock_->wall();
    const auto found = doc_.books.find(slot.id);
    const auto own = found != doc_.books.end() ? found->second.state : StateRecord{};
    const auto day_mark = found != doc_.books.end() ? found->second.day : DayMark{};
    const auto limits = found != doc_.books.end() ? found->second.limits : LimitRow{};
    const auto total = internal::add(slot.last.realized, slot.last.unrealized);
    const auto window = [&](std::uint64_t ns, bool past) {
        const auto seen = slot.speed.change(now, ns);
        return WindowView{.change = seen.change, .covered_s = seen.covered_ns / internal::k_s, .past_level = past};
    };
    return {.id = slot.id,
            .name = kernel_->name_of(slot.id),
            .own = own,
            .state = oms::stricter(doc_.desk.state, own.state),
            .open_orders = kernel_->open_orders(slot.id),
            .row = row_view(limits, slot.resolved, limits.loss_action.has_value()),
            .pnl = {.realized = slot.last.realized,
                    .unrealized = slot.last.unrealized,
                    .total = total,
                    .m1 = window(internal::k_m1, slot.past_1m),
                    .m5 = window(internal::k_m5, slot.past_5m),
                    .h1 = window(internal::k_h1, slot.past_1h),
                    .day = day_mark.day == 0 ? FixedE9::from_raw(0) : total - day_mark.start_pnl,
                    .past_daily = slot.past_day,
                    .positions = slot.last.positions,
                    .unmarked = slot.last.unmarked,
                    .unvalued = slot.last.unvalued}};
}

DeskView RiskDesk::view() {
    sync_books();
    DeskView out{.capital = doc_.capital,
                 .capital_by = doc_.capital_by,
                 .capital_at = doc_.capital_at,
                 .reported = reported_,
                 .capital_warning = warning_text(),
                 .desk = doc_.desk,
                 .defaults = row_view(doc_.defaults,
                                      resolve(kernel_->base_defaults(), doc_.defaults, {}, doc_.capital),
                                      doc_.defaults.loss_action.has_value())};
    for (const auto& [id, slot] : slots_) {
        out.books.push_back(book_view_of(*slot));
    }
    std::ranges::sort(out.books, {}, &BookView::id);
    return out;
}

std::optional<BookView> RiskDesk::book_view(std::string_view id_or_name) {
    sync_books();
    BookId id{};
    const auto* end = id_or_name.data() + id_or_name.size();
    if (const auto parsed = std::from_chars(id_or_name.data(), end, id);
        parsed.ec == std::errc{} && parsed.ptr == end) {
        const auto found = slots_.find(id);
        return found == slots_.end() ? std::nullopt : std::optional{book_view_of(*found->second)};
    }
    for (const auto& [known, slot] : slots_) {
        if (kernel_->name_of(known) == id_or_name) {
            return book_view_of(*slot);
        }
    }
    return std::nullopt;
}

void RiskDesk::record(std::string_view by, std::string_view action, std::string_view subject, std::string_view detail) {
    audit_->append({.ts = clock_->wall(), .actor = by, .action = action, .subject = subject, .detail_json = detail});
}

void RiskDesk::notify(
    NoticeLevel level, std::string_view kind, std::string_view subject, std::string_view text, UnixNanos at) {
    notifier_->notify({.level = level, .kind = kind, .subject = subject, .text = text, .at = at});
}

} // namespace raikiri::bookrisk
