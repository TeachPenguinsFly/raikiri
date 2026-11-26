#include <raikiri/admin/terms.hpp>

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <glaze/glaze.hpp>

namespace raikiri::admin {

struct MaintenanceTierBody {
    std::string notional_floor;
    std::optional<std::string> notional_cap{};
    std::string maintenance_margin_rate;
    std::string maintenance_amount;
    std::optional<std::string> max_leverage{};
};

struct TradingTermsBody {
    std::string account;
    std::string instrument_id;
    std::string maker_fee;
    std::string taker_fee;
    std::string fee_source;
    std::vector<MaintenanceTierBody> maintenance_tiers;
    std::string tiers_source;
    std::uint64_t as_of{0};
};

namespace {

constexpr glz::opts k_opts{.skip_null_members = true};
constexpr std::string_view k_prefix = "/v1/accounts/";
constexpr std::string_view k_suffix = "/terms";

MarginError fault_error(MarginFault fault) {
    switch (fault) {
    case MarginFault::GatewayGone:
        return {503, "gateway is not answering", "the instrument is listed but its gateway is down"};
    case MarginFault::NotServed:
        return {501, "this gateway serves no terms calls", "it was built before them"};
    case MarginFault::RingHeld:
        return {503, "another admind holds this gateway", "only the process holding the desk lease may ask"};
    case MarginFault::RingFull:
        return {503, "gateway is not draining requests", "worth retrying"};
    case MarginFault::NoAnswer:
        break;
    }
    return {504, "no answer from the gateway in time", "worth retrying"};
}

MarginError answer_error(const TermsAnswer& answer) {
    if (answer.outcome == TermsOutcome::Unsupported) {
        return {501, "not implemented for this venue", answer.reason};
    }
    if (answer.cls == ErrorClass::RateLimited) {
        return {429, "rate limited", answer.reason};
    }
    return {502, "the venue call did not complete", answer.reason};
}

http::Response render(const std::expected<TermsView, MarginError>& result) {
    if (!result.has_value()) {
        return http::problem(result.error().status, result.error().title, result.error().detail);
    }
    const auto& terms = result->terms;
    TradingTermsBody body{.account = result->account,
                          .instrument_id = std::to_string(result->instrument.packed),
                          .maker_fee = decimal_text(terms.maker_fee),
                          .taker_fee = decimal_text(terms.taker_fee),
                          .fee_source = std::string{to_string(terms.fee_source)},
                          .maintenance_tiers = {},
                          .tiers_source = std::string{to_string(terms.tiers_source)},
                          .as_of = result->as_of.ns / 1'000'000ULL};
    body.maintenance_tiers.reserve(terms.tiers.size());
    for (const auto& tier : terms.tiers) {
        MaintenanceTierBody row{.notional_floor = decimal_text(tier.floor),
                                .notional_cap = std::nullopt,
                                .maintenance_margin_rate = decimal_text(tier.rate),
                                .maintenance_amount = decimal_text(tier.amount),
                                .max_leverage = std::nullopt};
        if (tier.cap > 0) {
            row.notional_cap = decimal_text(tier.cap);
        }
        if (tier.max_leverage > 0) {
            row.max_leverage = decimal_text(tier.max_leverage);
        }
        body.maintenance_tiers.push_back(std::move(row));
    }
    std::string out;
    if (const auto ec = glz::write<k_opts>(body, out); ec) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(out)};
}

} // namespace

std::string decimal_text(double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    // twelve places hold any rate or bound a venue states and drop the noise of a product
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%.12f", value);
    std::string out{buf, n > 0 ? static_cast<std::size_t>(n) : 0U};
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (out.back() == '.') {
            out.pop_back();
        }
    }
    if (out.empty() || out == "-0") {
        return "0";
    }
    return out;
}

struct ShmTermsGateway::Waiting {
    std::uint64_t id{0};
    std::chrono::steady_clock::time_point deadline;
    shm::TermsReplyReader reader;
    TermsAnswer answer{};
    std::uint8_t next_part{0};
};

ShmTermsGateway::ShmTermsGateway(const Registry& registry,
                                 std::function<MonoNanos()> now,
                                 std::chrono::milliseconds wait)
    : registry_{&registry}, now_{std::move(now)}, wait_{wait} {}

ShmTermsGateway::~ShmTermsGateway() = default;

std::optional<MarginTarget> ShmTermsGateway::target(InstrumentId instrument) const {
    const auto row = registry_->catalogue_row(instrument);
    if (!row.has_value() || row->instrument == nullptr || row->gateway == nullptr) {
        return std::nullopt;
    }
    return MarginTarget{.account = row->gateway->name,
                        .symbol = std::string{base_of(*row->instrument).exchange_symbol.view()}};
}

double ShmTermsGateway::price(InstrumentId instrument) const {
    const auto row = registry_->catalogue_row(instrument);
    if (!row.has_value() || row->instrument == nullptr || row->gateway == nullptr) {
        return 0.0;
    }
    const auto& base = base_of(*row->instrument);
    const auto* table = row->gateway->channels.bbo;
    const std::size_t cell = instrument.symbol().raw;
    Bbo bbo{};
    if (table == nullptr || cell >= shm::k_bbo_cells || table->read(cell, bbo) == 0 || bbo.bid.is_undef() ||
        bbo.ask.is_undef()) {
        return 0.0;
    }
    const auto bid = static_cast<double>(base.ticks_to_price(bbo.bid).raw) / 1e9;
    const auto ask = static_cast<double>(base.ticks_to_price(bbo.ask).raw) / 1e9;
    return bid > 0 && ask > 0 ? (bid + ask) / 2 : 0.0;
}

std::expected<void, MarginFault> ShmTermsGateway::send(TermsRequest& request) {
    const auto row = registry_->catalogue_row(request.instrument);
    if (!row.has_value() || row->gateway == nullptr || !row->gateway->client.owner_alive(now_().ns)) {
        return std::unexpected{MarginFault::GatewayGone};
    }
    const auto& gateway = *row->gateway;
    const auto* replies = gateway.channels.terms_replies;
    if (gateway.channels.terms_requests == nullptr || replies == nullptr ||
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
                                                     .reader = shm::TermsReplyReader{replies}});
    shm::TermsRequestProducer producer{gateway.channels.terms_requests};
    if (!producer.try_push(request).has_value()) {
        return std::unexpected{MarginFault::RingFull};
    }
    waiting_.push_back(std::move(waiting));
    return {};
}

std::expected<std::optional<TermsAnswer>, MarginFault> ShmTermsGateway::collect(std::uint64_t id) {
    const auto it = std::ranges::find_if(waiting_, [id](const auto& waiting) { return waiting->id == id; });
    if (it == waiting_.end()) {
        return std::unexpected{MarginFault::NoAnswer};
    }
    auto& waiting = **it;
    TermsReply reply{};
    for (;;) {
        const auto polled = waiting.reader.poll(reply);
        if (polled == shm::PollResult::Empty) {
            break;
        }
        if (polled != shm::PollResult::Item || reply.id != id) {
            continue;
        }
        // a part out of turn means one was lost to a lap: the answer cannot be put together
        if (reply.part != waiting.next_part || reply.count > k_terms_tiers_per_part) {
            waiting_.erase(it);
            return std::unexpected{MarginFault::NoAnswer};
        }
        auto& answer = waiting.answer;
        if (reply.part == 0) {
            answer.outcome = reply.outcome;
            answer.cls = reply.cls;
            answer.reason = std::string{reply.reason()};
            answer.terms.maker_fee = reply.maker_fee;
            answer.terms.taker_fee = reply.taker_fee;
            answer.terms.fee_source = reply.fee_source;
            answer.terms.tiers_source = reply.tiers_source;
            answer.terms.basis = reply.basis;
        }
        answer.terms.tiers.insert(answer.terms.tiers.end(), reply.tiers, reply.tiers + reply.count);
        ++waiting.next_part;
        if (waiting.next_part >= reply.parts) {
            auto done = std::move(answer);
            waiting_.erase(it);
            return std::optional<TermsAnswer>{std::move(done)};
        }
    }
    if (std::chrono::steady_clock::now() >= waiting.deadline) {
        waiting_.erase(it);
        return std::unexpected{MarginFault::NoAnswer};
    }
    return std::optional<TermsAnswer>{};
}

TermsDesk::TermsDesk(TermsGateway& gateway, std::function<UnixNanos()> wall)
    : gateway_{&gateway}, wall_{std::move(wall)} {}

TermsView TermsDesk::view(std::string_view account, InstrumentId instrument, const Kept& kept) const {
    TermsView out{.account = std::string{account}, .instrument = instrument, .terms = kept.terms, .as_of = kept.as_of};
    if (out.terms.basis == TierBasis::BaseQty) {
        // priced at each answer, so the kept tiers follow the market between venue reads
        const double price = gateway_->price(instrument);
        if (price > 0) {
            for (auto& tier : out.terms.tiers) {
                tier.floor *= price;
                tier.cap *= price;
            }
            out.terms.basis = TierBasis::Notional;
        } else {
            out.terms.tiers.clear();
            out.terms.tiers_source = TiersSource::Default;
        }
    }
    return out;
}

void TermsDesk::read(std::string_view account, InstrumentId instrument, TermsDone done) {
    const auto target = gateway_->target(instrument);
    if (!target.has_value()) {
        done(std::unexpected{MarginError{404, "no such instrument", std::to_string(instrument.packed)}});
        return;
    }
    if (target->account != account) {
        done(std::unexpected{
            MarginError{404, "instrument is not listed on this account", "it belongs to " + target->account}});
        return;
    }
    const auto now = wall_();
    if (const auto it = kept_.find(instrument.packed); it != kept_.end()) {
        if (now.ns < it->second.as_of.ns + k_terms_ttl_ns) {
            done(view(account, instrument, it->second));
            return;
        }
        kept_.erase(it);
    }
    // one venue read serves everyone who asks meanwhile
    if (const auto it = std::ranges::find(waiting_, instrument, &Waiting::instrument); it != waiting_.end()) {
        it->done.push_back(std::move(done));
        return;
    }
    TermsRequest request{.id = 0, .instrument = instrument, .ts_created = now};
    if (const auto sent = gateway_->send(request); !sent.has_value()) {
        done(std::unexpected{fault_error(sent.error())});
        return;
    }
    Waiting waiting{.id = request.id, .account = std::string{account}, .instrument = instrument, .done = {}};
    waiting.done.push_back(std::move(done));
    waiting_.push_back(std::move(waiting));
}

void TermsDesk::pump() {
    // a finished entry is taken out before its callbacks run, which may ask again
    for (std::size_t i = 0; i < waiting_.size();) {
        auto collected = gateway_->collect(waiting_[i].id);
        if (collected.has_value() && !collected->has_value()) {
            ++i;
            continue;
        }
        auto waiting = std::move(waiting_[i]);
        waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
        if (collected.has_value()) {
            finish(waiting, std::move(**collected));
        } else {
            finish(waiting, std::unexpected{collected.error()});
        }
    }
}

void TermsDesk::finish(Waiting& waiting, const std::expected<TermsAnswer, MarginFault>& answer) {
    std::expected<TermsView, MarginError> result{};
    if (!answer.has_value()) {
        result = std::unexpected{fault_error(answer.error())};
    } else if (answer->outcome != TermsOutcome::Done) {
        result = std::unexpected{answer_error(*answer)};
    } else {
        const auto now = wall_();
        if (kept_.size() >= k_terms_cache_rows) {
            std::erase_if(kept_, [now](const auto& row) { return now.ns >= row.second.as_of.ns + k_terms_ttl_ns; });
        }
        if (kept_.size() >= k_terms_cache_rows) {
            // still full of fresh rows: the oldest goes
            kept_.erase(std::ranges::min_element(kept_, {}, [](const auto& row) { return row.second.as_of.ns; }));
        }
        const auto& kept = kept_[waiting.instrument.packed] = Kept{.terms = answer->terms, .as_of = now};
        result = view(waiting.account, waiting.instrument, kept);
    }
    for (const auto& done : waiting.done) {
        done(result);
    }
}

bool TermsRoutes::operator()(const http::Request& req, const MarginRespond& respond) const {
    if (!req.path.starts_with(k_prefix) || !req.path.ends_with(k_suffix) ||
        req.path.size() <= k_prefix.size() + k_suffix.size()) {
        return false;
    }
    const auto account = req.path.substr(k_prefix.size(), req.path.size() - k_prefix.size() - k_suffix.size());
    if (account.find('/') != std::string_view::npos) {
        return false;
    }
    if (req.method != "GET") {
        respond(http::problem(405, "method not allowed", "this path takes GET"));
        return true;
    }
    const auto text = http::query_param(req.query, "instrument_id");
    std::uint64_t packed = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), packed);
    if (text.empty() || ec != std::errc{} || stop != text.data() + text.size()) {
        respond(http::problem(400, "instrument_id is required", "digits"));
        return true;
    }
    desk_->read(account, InstrumentId{packed}, [respond](const auto& result) { respond(render(result)); });
    return true;
}

} // namespace raikiri::admin
