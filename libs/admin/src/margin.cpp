#include <raikiri/admin/margin.hpp>

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <glaze/glaze.hpp>

namespace raikiri::admin {

struct LeverageBody {
    std::string instrument_id;
    std::optional<std::int64_t> leverage{};
    std::optional<std::string> margin_mode{};
};

struct LeverageStateBody {
    std::string account;
    std::string instrument_id;
    std::optional<std::uint32_t> leverage{};
    std::optional<std::string> margin_mode{};
    std::optional<std::uint32_t> max_leverage{};
    std::optional<std::string> scope{};
};

struct MarginStateEntry {
    std::optional<std::uint32_t> leverage{};
    std::optional<std::string> margin_mode{};
};

struct MarginJournalEntry {
    std::string account;
    std::string instrument_id;
    std::string symbol;
    MarginStateEntry requested{};
    std::optional<MarginStateEntry> before{};
    std::optional<MarginStateEntry> after{};
    std::string outcome;
    std::optional<std::string> reason{};
};

namespace {

constexpr glz::opts k_opts{.skip_null_members = true};
constexpr std::string_view k_prefix = "/v1/accounts/";
constexpr std::string_view k_suffix = "/leverage";

MarginStateEntry entry_of(const MarginState& state) {
    MarginStateEntry out{};
    if (state.leverage != 0) {
        out.leverage = state.leverage;
    }
    if (state.mode != MarginMode::Unknown) {
        out.margin_mode = std::string{to_string(state.mode)};
    }
    return out;
}

MarginError fault_error(MarginFault fault) {
    switch (fault) {
    case MarginFault::GatewayGone:
        return {503, "gateway is not answering", "the instrument is listed but its gateway is down"};
    case MarginFault::NotServed:
        return {501,
                "this gateway serves no account calls",
                "it was built before leverage calls, or runs without credentials"};
    case MarginFault::RingHeld:
        return {503, "another admind holds this gateway", "only the process holding the desk lease may ask"};
    case MarginFault::RingFull:
        return {503, "gateway is not draining requests", "worth retrying"};
    case MarginFault::NoAnswer:
        break;
    }
    return {504,
            "no answer from the gateway in time",
            "the change may still have reached the venue; read the state again before retrying"};
}

std::string_view outcome_text(MarginOutcome outcome) noexcept {
    switch (outcome) {
    case MarginOutcome::Done:
        return "done";
    case MarginOutcome::Refused:
        return "refused";
    case MarginOutcome::Unsupported:
        return "unsupported";
    case MarginOutcome::Failed:
        break;
    }
    return "failed";
}

MarginError reply_error(const MarginReply& reply) {
    std::string detail{reply.reason()};
    if (reply.venue_code != 0 && reply.outcome == MarginOutcome::Refused) {
        detail += " (venue code " + std::to_string(reply.venue_code) + ")";
    }
    switch (reply.outcome) {
    case MarginOutcome::Refused:
        return {409, "the venue refused", std::move(detail)};
    case MarginOutcome::Unsupported:
        return {501, "not implemented for this venue", std::move(detail)};
    case MarginOutcome::Done:
    case MarginOutcome::Failed:
        break;
    }
    if (reply.cls == ErrorClass::RateLimited) {
        return {429, "rate limited", std::move(detail)};
    }
    return {502, "the venue call did not complete", std::move(detail)};
}

http::Response answer(const std::expected<MarginView, MarginError>& result) {
    if (!result.has_value()) {
        return http::problem(result.error().status, result.error().title, result.error().detail);
    }
    LeverageStateBody body{.account = result->account, .instrument_id = std::to_string(result->instrument.packed)};
    const auto state = entry_of(result->state);
    body.leverage = state.leverage;
    body.margin_mode = state.margin_mode;
    if (result->state.max_leverage != 0) {
        body.max_leverage = result->state.max_leverage;
    }
    if (result->scope != MarginScope::Unknown) {
        body.scope = std::string{to_string(result->scope)};
    }
    std::string out;
    if (const auto ec = glz::write<k_opts>(body, out); ec) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(out)};
}

std::optional<InstrumentId> read_instrument(std::string_view text) {
    std::uint64_t packed = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), packed);
    if (text.empty() || ec != std::errc{} || stop != text.data() + text.size()) {
        return std::nullopt;
    }
    return InstrumentId{packed};
}

} // namespace

struct ShmMarginGateway::Waiting {
    std::uint64_t id{0};
    std::chrono::steady_clock::time_point deadline;
    shm::MarginReplyReader reader;
};

ShmMarginGateway::ShmMarginGateway(const Registry& registry,
                                   std::function<MonoNanos()> now,
                                   std::chrono::milliseconds wait)
    : registry_{&registry}, now_{std::move(now)}, wait_{wait} {}

ShmMarginGateway::~ShmMarginGateway() = default;

std::optional<MarginTarget> ShmMarginGateway::target(InstrumentId instrument) const {
    const auto row = registry_->catalogue_row(instrument);
    if (!row.has_value() || row->instrument == nullptr || row->gateway == nullptr) {
        return std::nullopt;
    }
    return MarginTarget{.account = row->gateway->name,
                        .symbol = std::string{base_of(*row->instrument).exchange_symbol.view()}};
}

std::expected<void, MarginFault> ShmMarginGateway::send(MarginRequest& request) {
    const auto row = registry_->catalogue_row(request.instrument);
    if (!row.has_value() || row->gateway == nullptr || !row->gateway->client.owner_alive(now_().ns)) {
        return std::unexpected{MarginFault::GatewayGone};
    }
    const auto& gateway = *row->gateway;
    const auto* replies = gateway.channels.margin_replies;
    if (gateway.channels.margin_requests == nullptr || replies == nullptr ||
        replies->published.load(std::memory_order_acquire) == 0) {
        return std::unexpected{MarginFault::NotServed};
    }
    if (!gateway.lease.has_value()) {
        return std::unexpected{MarginFault::RingHeld};
    }
    request.id = (static_cast<std::uint64_t>(::getpid()) << 32U) | (++seq_ & 0xFFFF'FFFFU);

    // placed before the push, so the reply cannot pass it
    auto waiting = std::make_unique<Waiting>(Waiting{.id = request.id,
                                                     .deadline = std::chrono::steady_clock::now() + wait_,
                                                     .reader = shm::MarginReplyReader{replies}});
    shm::MarginRequestProducer producer{gateway.channels.margin_requests};
    if (!producer.try_push(request).has_value()) {
        return std::unexpected{MarginFault::RingFull};
    }
    waiting_.push_back(std::move(waiting));
    return {};
}

std::expected<std::optional<MarginReply>, MarginFault> ShmMarginGateway::collect(std::uint64_t id) {
    const auto it = std::ranges::find_if(waiting_, [id](const auto& waiting) { return waiting->id == id; });
    if (it == waiting_.end()) {
        return std::unexpected{MarginFault::NoAnswer};
    }
    auto& waiting = **it;
    MarginReply reply{};
    for (;;) {
        const auto polled = waiting.reader.poll(reply);
        if (polled == shm::PollResult::Empty) {
            break;
        }
        if (polled == shm::PollResult::Item && reply.id == id) {
            waiting_.erase(it);
            return std::optional<MarginReply>{reply};
        }
    }
    if (std::chrono::steady_clock::now() >= waiting.deadline) {
        waiting_.erase(it);
        return std::unexpected{MarginFault::NoAnswer};
    }
    return std::optional<MarginReply>{};
}

MarginDesk::MarginDesk(MarginGateway& gateway, audit::Sink* journal, std::function<UnixNanos()> wall)
    : gateway_{&gateway}, journal_{journal}, wall_{std::move(wall)} {}

std::expected<MarginTarget, MarginError> MarginDesk::resolve(std::string_view account, InstrumentId instrument) const {
    auto target = gateway_->target(instrument);
    if (!target.has_value()) {
        return std::unexpected{MarginError{404, "no such instrument", std::to_string(instrument.packed)}};
    }
    if (target->account != account) {
        return std::unexpected{
            MarginError{404, "instrument is not listed on this account", "it belongs to " + target->account}};
    }
    const auto market = instrument.market();
    if (market != MarketType::Perp && market != MarketType::Future) {
        return std::unexpected{MarginError{
            422, "no leverage on this market", "leverage and margin mode belong to perpetuals and futures"}};
    }
    return std::move(*target);
}

void MarginDesk::read(std::string_view account, InstrumentId instrument, MarginDone done) {
    auto target = resolve(account, instrument);
    if (!target.has_value()) {
        done(std::unexpected{target.error()});
        return;
    }
    MarginRequest request{.instrument = instrument, .ts_created = wall_(), .op = MarginOp::Read};
    if (const auto sent = gateway_->send(request); !sent.has_value()) {
        done(std::unexpected{fault_error(sent.error())});
        return;
    }
    waiting_.push_back(
        {.id = request.id, .target = std::move(*target), .instrument = instrument, .change = std::nullopt, .actor = {}, .done = std::move(done)});
}

void MarginDesk::set(std::string_view account,
                     InstrumentId instrument,
                     const MarginChange& change,
                     std::string_view actor,
                     MarginDone done) {
    auto target = resolve(account, instrument);
    if (!target.has_value()) {
        done(std::unexpected{target.error()});
        return;
    }
    if (!change.leverage.has_value() && !change.mode.has_value()) {
        done(std::unexpected{MarginError{422, "nothing to change", "send leverage, margin_mode or both"}});
        return;
    }
    if (change.leverage.has_value() && (*change.leverage < 1 || *change.leverage > k_max_leverage_asked)) {
        done(std::unexpected{MarginError{422, "bad leverage", "a whole number from 1 to 1000"}});
        return;
    }
    if (change.mode.has_value() && *change.mode == MarginMode::Unknown) {
        done(std::unexpected{MarginError{422, "bad margin_mode", "expected cross or isolated"}});
        return;
    }
    if (journal_ == nullptr) {
        done(std::unexpected{MarginError{503, "no journal", "a change that leaves no record is not made"}});
        return;
    }
    MarginRequest request{.instrument = instrument,
                          .ts_created = wall_(),
                          .leverage = change.leverage.value_or(0),
                          .op = MarginOp::Set,
                          .mode = change.mode.value_or(MarginMode::Unknown)};
    Waiting waiting{.target = std::move(*target),
                    .instrument = instrument,
                    .change = change,
                    .actor = std::string{actor},
                    .done = std::move(done)};
    if (const auto sent = gateway_->send(request); !sent.has_value()) {
        finish(waiting, std::unexpected{sent.error()});
        return;
    }
    waiting.id = request.id;
    waiting_.push_back(std::move(waiting));
}

void MarginDesk::pump() {
    // a finished entry is taken out before its callback runs, which may ask again
    for (std::size_t i = 0; i < waiting_.size();) {
        const auto collected = gateway_->collect(waiting_[i].id);
        if (collected.has_value() && !collected->has_value()) {
            ++i;
            continue;
        }
        auto waiting = std::move(waiting_[i]);
        waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
        if (collected.has_value()) {
            finish(waiting, **collected);
        } else {
            finish(waiting, std::unexpected{collected.error()});
        }
    }
}

void MarginDesk::finish(Waiting& waiting, const std::expected<MarginReply, MarginFault>& reply) {
    if (!waiting.change.has_value()) {
        if (!reply.has_value()) {
            waiting.done(std::unexpected{fault_error(reply.error())});
        } else if (reply->outcome != MarginOutcome::Done) {
            waiting.done(std::unexpected{reply_error(*reply)});
        } else {
            waiting.done(MarginView{.account = waiting.target.account,
                                    .instrument = waiting.instrument,
                                    .state = reply->after,
                                    .scope = reply->scope});
        }
        return;
    }
    const auto& change = *waiting.change;
    const auto& target = waiting.target;
    MarginJournalEntry entry{
        .account = target.account,
        .instrument_id = std::to_string(waiting.instrument.packed),
        .symbol = target.symbol,
        .requested = {.leverage = change.leverage, .margin_mode = change.mode.transform([](MarginMode mode) {
                          return std::string{to_string(mode)};
                      })},
        .outcome = "no_answer"};
    std::expected<MarginView, MarginError> result{};
    if (!reply.has_value()) {
        result = std::unexpected{fault_error(reply.error())};
        entry.reason = result.error().title;
        // the request never left this process
        if (reply.error() != MarginFault::NoAnswer) {
            entry.outcome = "not_sent";
        }
    } else {
        entry.before = entry_of(reply->before);
        entry.after = entry_of(reply->after);
        entry.outcome = std::string{outcome_text(reply->outcome)};
        if (reply->outcome == MarginOutcome::Done) {
            result = MarginView{.account = target.account,
                                .instrument = waiting.instrument,
                                .state = reply->after,
                                .scope = reply->scope};
        } else {
            result = std::unexpected{reply_error(*reply)};
            entry.reason = result.error().detail;
        }
    }
    std::string detail;
    if (const auto ec = glz::write<k_opts>(entry, detail); ec) {
        detail = "{}";
    }
    const std::string subject = target.account + "/" + target.symbol;
    journal_->append(
        {.ts = wall_(), .actor = waiting.actor, .action = "margin.set", .subject = subject, .detail_json = detail});
    waiting.done(std::move(result));
}

bool MarginRoutes::operator()(const http::Request& req, const MarginRespond& respond) const {
    if (!req.path.starts_with(k_prefix) || !req.path.ends_with(k_suffix) ||
        req.path.size() <= k_prefix.size() + k_suffix.size()) {
        return false;
    }
    const auto account = req.path.substr(k_prefix.size(), req.path.size() - k_prefix.size() - k_suffix.size());
    if (account.find('/') != std::string_view::npos) {
        return false;
    }
    if (req.method == "GET") {
        const auto text = http::query_param(req.query, "instrument_id");
        const auto instrument = read_instrument(text);
        if (!instrument.has_value()) {
            respond(http::problem(400, "instrument_id is required", "digits"));
            return true;
        }
        desk_->read(account, *instrument, [respond](const auto& result) { respond(answer(result)); });
        return true;
    }
    if (req.method != "PUT") {
        respond(http::problem(405, "method not allowed", "this path takes GET and PUT"));
        return true;
    }
    LeverageBody body{};
    if (const auto ec = glz::read_json(body, req.body); ec) {
        respond(http::problem(400, "unreadable body", glz::format_error(ec, req.body)));
        return true;
    }
    const auto instrument = read_instrument(body.instrument_id);
    if (!instrument.has_value()) {
        respond(http::problem(422, "bad instrument_id", "expected the packed id as digits"));
        return true;
    }
    MarginChange change{};
    if (body.leverage.has_value()) {
        if (*body.leverage < 1 || *body.leverage > k_max_leverage_asked) {
            respond(http::problem(422, "bad leverage", "a whole number from 1 to 1000"));
            return true;
        }
        change.leverage = static_cast<std::uint32_t>(*body.leverage);
    }
    if (body.margin_mode.has_value()) {
        if (*body.margin_mode == "cross") {
            change.mode = MarginMode::Cross;
        } else if (*body.margin_mode == "isolated") {
            change.mode = MarginMode::Isolated;
        } else {
            respond(http::problem(422, "bad margin_mode", "expected cross or isolated"));
            return true;
        }
    }
    const std::string_view actor = req.login.empty() ? std::string_view{"admind"} : req.login;
    desk_->set(account, *instrument, change, actor, [respond](const auto& result) { respond(answer(result)); });
    return true;
}

} // namespace raikiri::admin
