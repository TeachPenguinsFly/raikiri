#include <raikiri/joe/desk.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace raikiri::joe {
namespace internal {

[[nodiscard]] std::string decimal(FixedE9 value) {
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

[[nodiscard]] std::optional<FixedE9> touch(std::span<const VenueBook> books, Side taker) {
    std::optional<FixedE9> best;
    for (const auto& book : books) {
        if (book.levels.empty()) {
            continue;
        }
        const auto price = book.levels.front().price;
        if (!best.has_value() || (taker == Side::Buy ? price < *best : price > *best)) {
            best = price;
        }
    }
    return best;
}

// the run an open-ended parent's mid buffer is shaped for; past it the oldest samples go
constexpr std::uint64_t k_open_run_ns = 3600ULL * 1'000'000'000ULL;

constexpr std::string_view k_stopped = "stopped";

// a slice repeats its siblings on purpose; only the same slice sent twice is a duplicate
[[nodiscard]] std::uint64_t repeat_key(const Parent& parent) noexcept {
    return (parent.id() << 32) | ((parent.child_count() + 1) & 0xFFFF'FFFFULL);
}

[[nodiscard]] OrderOrigin origin_of(const Parent& parent) noexcept {
    return {.parent = parent.id(),
            .external_ref = ExternalRef::from(parent.spec().external_ref).value_or(ExternalRef{})};
}

[[nodiscard]] Refusal refusal(TransitionError error) {
    return {.error = DeskError::Conflict, .detail = std::string{to_string(error)}};
}

} // namespace internal

Desk::Desk(
    const MarketView& market, OrderSink& sink, const Clock& clock, const VolumeProfile& recorded, DeskConfig config)
    : market_{&market}, sink_{&sink}, clock_{&clock}, recorded_{&recorded}, config_{config},
      next_id_{config.first_parent_id} {}

std::expected<std::uint64_t, Refusal> Desk::start(ParentSpec spec) {
    const LogChanges logged{*this};
    if (const auto valid = validate(spec); !valid.has_value()) {
        return std::unexpected{Refusal{.error = DeskError::BadSpec, .detail = std::string{to_string(valid.error())}}};
    }
    if (const auto* held = find_tag(spec.client_tag); held != nullptr) {
        return held->id();
    }
    if (!sink_->running()) {
        return std::unexpected{Refusal{.error = DeskError::NotRunning, .detail = "joe is not running in strategyd"}};
    }
    if (spec.book == 0) {
        spec.book = config_.own_book;
    }
    const auto now = clock_->wall();
    auto& parent = parents_.emplace_back(next_id_++, spec, now);
    const auto id = parent.id();
    if (log_ != nullptr) {
        log_->record({.kind = ParentRecord::Kind::Created, .parent = id, .ts = now, .spec = spec});
    }
    begin(parent, now);
    prune();
    return id;
}

std::expected<void, Refusal> Desk::cancel(std::uint64_t id) {
    const LogChanges logged{*this};
    auto* parent = find_mut(id);
    if (parent == nullptr) {
        return std::unexpected{Refusal{.error = DeskError::NoSuchParent, .detail = std::to_string(id)}};
    }
    if (const auto done = parent->cancel(clock_->wall()); !done.has_value()) {
        return std::unexpected{internal::refusal(done.error())};
    }
    cancel_children(*parent);
    return {};
}

std::expected<void, Refusal> Desk::pause(std::uint64_t id) {
    const LogChanges logged{*this};
    auto* parent = find_mut(id);
    if (parent == nullptr) {
        return std::unexpected{Refusal{.error = DeskError::NoSuchParent, .detail = std::to_string(id)}};
    }
    if (const auto done = parent->pause(clock_->wall()); !done.has_value()) {
        return std::unexpected{internal::refusal(done.error())};
    }
    cancel_children(*parent);
    return {};
}

std::expected<void, Refusal> Desk::resume(std::uint64_t id) {
    const LogChanges logged{*this};
    auto* parent = find_mut(id);
    if (parent == nullptr) {
        return std::unexpected{Refusal{.error = DeskError::NoSuchParent, .detail = std::to_string(id)}};
    }
    if (!sink_->running()) {
        return std::unexpected{Refusal{.error = DeskError::NotRunning, .detail = "joe is not running in strategyd"}};
    }
    if (const auto done = parent->resume(clock_->wall()); !done.has_value()) {
        return std::unexpected{internal::refusal(done.error())};
    }
    return {};
}

const Parent* Desk::find(std::uint64_t id) const noexcept {
    const auto it = std::ranges::find_if(parents_, [id](const Parent& p) { return p.id() == id; });
    return it == parents_.end() ? nullptr : &*it;
}

const Parent* Desk::find_tag(std::string_view tag) const noexcept {
    if (tag.empty()) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(parents_, [tag](const Parent& p) { return p.spec().client_tag == tag; });
    return it == parents_.end() ? nullptr : &*it;
}

Parent* Desk::find_mut(std::uint64_t id) noexcept {
    const auto it = std::ranges::find_if(parents_, [id](const Parent& p) { return p.id() == id; });
    return it == parents_.end() ? nullptr : &*it;
}

void Desk::tick() {
    const LogChanges logged{*this};
    if (!sink_->running()) {
        return;
    }
    const auto now = clock_->wall();
    for (auto& parent : parents_) {
        if (parent.status() == Status::Pending) {
            begin(parent, now);
            continue;
        }
        if (parent.status() == Status::Working || parent.status() == Status::Paused) {
            sample(parent, now);
        }
        if (parent.status() == Status::Working) {
            work(parent, now);
        }
        chase_cancels(parent, now);
        parent.retire(now, config_.keep_children, config_.late_fill_ns);
    }
}

void Desk::begin(Parent& parent, UnixNanos now) {
    const auto& spec = parent.spec();
    const auto books = market_->books(spec.listing, spec.side);
    const auto taken = internal::touch(books, spec.side);
    if (!taken.has_value()) {
        if (now.ns - parent.created().ns > config_.grace_ns) {
            (void)parent.fail(now, "no venue that takes orders quotes this listing");
        }
        return;
    }
    const auto other = internal::touch(market_->books(spec.listing, opposite(spec.side)), opposite(spec.side));
    const auto arrival = other.has_value() ? FixedE9::from_raw((taken->raw + other->raw) / 2) : *taken;
    const auto qty = spec.qty.is_undef() || spec.qty.raw <= 0
                         ? FixedE9::from_raw(mul_div(spec.notional.raw, k_e9, arrival.raw))
                         : spec.qty;
    (void)parent.start(now, qty, arrival);
    open_trace(parent);
    (void)parent.trace().sample(now, arrival, FixedE9::undef());
    const auto seed = parent.id() ^ now.ns;
    switch (spec.algo) {
    case Algo::Sor:
        parent.plan({now}, {1});
        break;
    case Algo::Twap:
        parent.plan(slice_times(now, spec.horizon_ns, spec.slices, spec.jitter_bp, seed), flat_weights(spec.slices));
        break;
    case Algo::Vwap: {
        auto weights = spec.profile == Profile::U ? u_weights(spec.slices)
                       : spec.profile == Profile::Recorded
                           ? recorded_->weights(spec.listing, now, spec.horizon_ns, spec.slices)
                           : flat_weights(spec.slices);
        parent.plan(slice_times(now, spec.horizon_ns, spec.slices, spec.jitter_bp, seed), std::move(weights));
        break;
    }
    case Algo::Pov:
        break;
    case Algo::Iceberg:
    case Algo::Pegged:
        parent.slot().random = seed;
        break;
    }
    if (log_ != nullptr) {
        const auto times = parent.times();
        const auto weights = parent.weights();
        log_->record({.kind = ParentRecord::Kind::Started,
                      .parent = parent.id(),
                      .ts = now,
                      .qty = qty,
                      .arrival = arrival,
                      .times = {times.begin(), times.end()},
                      .weights = {weights.begin(), weights.end()},
                      .random = parent.slot().random});
    }
    work(parent, now);
}

void Desk::open_trace(Parent& parent) {
    const auto& spec = parent.spec();
    const auto run = (spec.algo == Algo::Sor                       ? 0
                      : resting(spec.algo) && spec.horizon_ns == 0 ? internal::k_open_run_ns
                                                                   : spec.horizon_ns) +
                     config_.grace_ns;
    const auto shape = trace_shape(run, config_.sample_ns, config_.trace_mids);
    parent.trace().open(shape.interval_ns, shape.mids, config_.trace_fills, config_.trace_reprices);
}

void Desk::log_child(const Parent& parent, const Child& child) {
    if (log_ != nullptr) {
        log_->record({.kind = ParentRecord::Kind::ChildSent, .parent = parent.id(), .ts = child.sent, .child = child});
    }
}

void Desk::log_changes() {
    if (log_ == nullptr) {
        return;
    }
    for (const auto& parent : parents_) {
        // pending to working is the started record's
        if (parent.status() == Status::Pending) {
            continue;
        }
        auto& logged = logged_.try_emplace(parent.id(), Status::Working).first->second;
        if (logged == parent.status()) {
            continue;
        }
        logged = parent.status();
        const auto at = terminal(parent.status()) ? parent.ended() : clock_->wall();
        log_->record({.kind = ParentRecord::Kind::StatusChanged,
                      .parent = parent.id(),
                      .ts = at,
                      .status = parent.status(),
                      .reason = halting_ ? std::string{internal::k_stopped} : std::string{parent.reason()}});
    }
    halting_ = false;
}

void Desk::restore(const ParentRecord& record) {
    using Kind = ParentRecord::Kind;
    if (record.kind == Kind::Created) {
        parents_.emplace_back(record.parent, record.spec, record.ts);
        next_id_ = std::max(next_id_, record.parent + 1);
        // a long journal holds far more finished parents than a desk keeps, and each event looks through them
        prune();
        return;
    }
    auto* parent = find_mut(record.parent);
    if (parent == nullptr) {
        return;
    }
    switch (record.kind) {
    case Kind::Created:
        break;
    case Kind::Started:
        (void)parent->start(record.ts, record.qty, record.arrival);
        open_trace(*parent);
        (void)parent->trace().sample(record.ts, record.arrival, FixedE9::undef());
        parent->plan(record.times, record.weights);
        parent->slot().random = record.random;
        break;
    case Kind::ChildSent:
        parent->add(record.child);
        parent->slot().changed = record.ts;
        parent->retire(record.ts, config_.keep_children, config_.late_fill_ns);
        break;
    case Kind::StatusChanged:
        logged_[parent->id()] = record.status;
        switch (record.status) {
        case Status::Pending:
            break;
        case Status::Working:
            (void)parent->resume(record.ts);
            break;
        case Status::Paused:
            (void)parent->pause(record.ts);
            // this process stopped, nobody asked it to pause, so a restart goes on
            if (record.reason == internal::k_stopped) {
                restart_paused_.push_back(parent->id());
            }
            break;
        case Status::Done:
            (void)parent->finish(record.ts);
            break;
        case Status::Cancelled:
            (void)parent->cancel(record.ts);
            break;
        case Status::Failed:
            (void)parent->fail(record.ts, record.reason);
            break;
        }
        break;
    }
}

void Desk::restore(const OrderEvent& event) {
    const auto id = event.client_order_id.raw;
    auto* parent = owner_of(id);
    if (parent == nullptr) {
        return;
    }
    const auto now = event.ts_init;
    const auto closes = [&](ChildEnd end) { (void)parent->close(now, id, end); };
    switch (event.type) {
    case OrderEventType::Accepted:
        (void)parent->ack(now, id);
        (void)parent->modified(now, id);
        break;
    case OrderEventType::Updated:
        (void)parent->modified(now, id);
        break;
    case OrderEventType::ModifyRejected:
        (void)parent->modify_refused(id);
        break;
    case OrderEventType::CancelRejected:
        parent->cancel_refused(id);
        break;
    case OrderEventType::Filled: {
        const auto scale = market_->scale(event.instrument);
        if (!scale.has_value() || event.qty.is_undef() || event.price.is_undef()) {
            break;
        }
        const auto left = event.leaves_qty.is_undef() ? FixedE9::undef() : scale->lot.scaled_by(event.leaves_qty.raw);
        (void)parent->fill(
            now, id, scale->lot.scaled_by(event.qty.raw), scale->tick.scaled_by(event.price.raw), event.fee, left);
        if (event.leaves_qty.raw == 0) {
            closes(ChildEnd::Filled);
        }
        break;
    }
    case OrderEventType::Canceled:
        closes(ChildEnd::Cancelled);
        break;
    case OrderEventType::Expired:
        closes(ChildEnd::Expired);
        break;
    case OrderEventType::Rejected:
        closes(ChildEnd::Rejected);
        break;
    case OrderEventType::Denied:
        closes(ChildEnd::Denied);
        break;
    case OrderEventType::ReconClosed:
        closes(ChildEnd::Lost);
        break;
    default:
        break;
    }
}

void Desk::finish_restore() {
    const auto now = clock_->wall();
    for (auto& parent : parents_) {
        if (parent.status() == Status::Working) {
            (void)parent.pause(now);
            logged_[parent.id()] = Status::Paused;
            restart_paused_.push_back(parent.id());
        }
    }
    prune();
}

std::size_t Desk::resume_restored() {
    const LogChanges logged{*this};
    const auto now = clock_->wall();
    std::size_t resumed = 0;
    for (const auto id : restart_paused_) {
        if (auto* parent = find_mut(id); parent != nullptr && parent->resume(now).has_value()) {
            ++resumed;
        }
    }
    restart_paused_.clear();
    return resumed;
}

void Desk::work(Parent& parent, UnixNanos now) {
    const auto& spec = parent.spec();
    if (resting(spec.algo)) {
        rest(parent, now);
        return;
    }
    const auto books = market_->books(spec.listing, spec.side);
    const auto working = parent.working();
    auto min_lot = std::numeric_limits<std::int64_t>::max();
    for (const auto& book : books) {
        min_lot = std::min(min_lot, book.lot.raw);
    }
    const auto left = parent.qty().raw - parent.filled().raw;
    if (left <= 0 || (!books.empty() && left < min_lot)) {
        if (working.raw == 0 && parent.filled().raw == 0) {
            (void)parent.fail(now, "the order is smaller than one lot on every venue");
        } else if (working.raw == 0) {
            (void)parent.finish(now);
        }
        return;
    }
    const auto horizon = spec.algo == Algo::Sor ? 0 : spec.horizon_ns;
    if (now.ns >= parent.started().ns + horizon + config_.grace_ns) {
        if (working.raw == 0) {
            (void)parent.fail(now, "the horizon passed with " + internal::decimal(FixedE9::from_raw(left)) + " left");
        } else {
            cancel_children(parent);
        }
        return;
    }
    if (books.empty()) {
        return;
    }
    if (const auto kids = parent.children(); !kids.empty() && now.ns - kids.back().sent.ns < config_.resend_ns) {
        return;
    }
    FixedE9 want{};
    if (spec.algo == Algo::Pov) {
        const auto target = pov_target(parent.qty(), parent.market_volume(), spec.pov_bp);
        const bool last = now.ns >= parent.started().ns + horizon;
        want = next_slice(last ? parent.qty() : target, parent.filled(), working, target, 100, true);
    } else {
        const auto count = fired(parent.times(), now);
        const auto target = cumulative_target(parent.qty(), parent.weights(), count);
        const auto planned = count == 0 ? FixedE9::from_raw(0) : slice_size(parent.qty(), parent.weights(), count - 1);
        want = next_slice(target, parent.filled(), working, planned, spec.catchup_pct, count >= parent.times().size());
    }
    if (want.raw < min_lot) {
        return;
    }
    send(parent, want, books, now);
}

void Desk::send(Parent& parent, FixedE9 want, std::span<const VenueBook> books, UnixNanos now) {
    const auto& spec = parent.spec();
    const auto planned = route(spec.side, want, spec.limit, books);
    bool too_big = !planned.legs.empty();
    for (const auto& leg : planned.legs) {
        const auto per_lot = value_of(leg.lot, leg.limit);
        const auto cap = per_lot.raw > 0 ? spec.max_child_notional.raw / per_lot.raw : 0;
        if (cap == 0) {
            continue;
        }
        too_big = false;
        const auto lots = std::min(leg.lots, cap);
        const auto sent = sink_->submit({.instrument = leg.instrument,
                                         .side = spec.side,
                                         .lots = lots,
                                         .limit = leg.limit,
                                         .tif = ChildTif::Ioc,
                                         .book = spec.book,
                                         .origin = internal::origin_of(parent),
                                         .repeat_key = internal::repeat_key(parent)});
        if (sent.has_value()) {
            parent.add({.id = *sent,
                        .instrument = leg.instrument,
                        .lot = leg.lot,
                        .lots = lots,
                        .limit = leg.limit,
                        .sent = now});
            log_child(parent, parent.children().back());
            parent.accepted();
            continue;
        }
        if (sent.error() == SinkError::NotRunning) {
            return;
        }
        if (sent.error() == SinkError::Risk && parent.refused() >= config_.max_refusals) {
            (void)parent.fail(now,
                              "the risk gate refused " + std::to_string(config_.max_refusals) + " children in a row");
            cancel_children(parent);
            return;
        }
    }
    if (too_big && parent.working().raw == 0) {
        (void)parent.fail(now, "one lot on every venue is worth more than max_child_notional");
    }
}

void Desk::sample(Parent& parent, UnixNanos now) {
    auto& trace = parent.trace();
    if (!trace.due(now)) {
        return;
    }
    const auto& spec = parent.spec();
    const auto ask = internal::touch(market_->books(spec.listing, Side::Buy), Side::Buy);
    const auto bid = internal::touch(market_->books(spec.listing, Side::Sell), Side::Sell);
    if (!ask.has_value() || !bid.has_value()) {
        return;
    }
    (void)trace.sample(now, FixedE9::from_raw((ask->raw + bid->raw) / 2), parent.market_vwap());
}

void Desk::rest(Parent& parent, UnixNanos now) {
    const auto& spec = parent.spec();
    const auto* live = parent.live();
    const auto asks = market_->books(spec.listing, Side::Buy);
    const auto bids = market_->books(spec.listing, Side::Sell);
    const auto& far = spec.side == Side::Buy ? asks : bids;
    const auto left = parent.qty().raw - parent.filled().raw;

    auto least_lot = live != nullptr ? live->lot.raw : std::numeric_limits<std::int64_t>::max();
    for (const auto& book : far) {
        least_lot = std::min(least_lot, book.lot.raw);
    }
    // with no book and no child the lot is unknown, which is not the same as nothing left
    const bool dust = least_lot != std::numeric_limits<std::int64_t>::max() && left < least_lot;
    const bool expired = spec.horizon_ns != 0 && now.ns >= parent.started().ns + spec.horizon_ns;
    if (left <= 0 || dust || expired) {
        if (live != nullptr) {
            pull(parent, *live, now);
        } else if (expired && left > 0) {
            (void)parent.fail(now, "the horizon passed with " + internal::decimal(FixedE9::from_raw(left)) + " left");
        } else if (parent.filled().raw == 0) {
            (void)parent.fail(now, "the order is smaller than one lot on every venue");
        } else {
            (void)parent.finish(now);
        }
        return;
    }
    if (live != nullptr && live->cancel_sent.ns != 0) {
        return;
    }

    const auto chosen = spec.venue != VenueId::Invalid
                            ? std::ranges::find_if(
                                  far, [&](const VenueBook& b) { return b.instrument.venue() == spec.venue; })
                            : far.end();
    const VenueBook* book = nullptr;
    if (live != nullptr) {
        const auto it = std::ranges::find(far, live->instrument, &VenueBook::instrument);
        book = it == far.end() ? nullptr : &*it;
    } else if (spec.venue == VenueId::Invalid) {
        book = best_venue(spec.side, far, spec.post_only);
    } else if (chosen != far.end()) {
        book = &*chosen;
    }
    if (book == nullptr) {
        if (live == nullptr && parent.children().empty() && now.ns - parent.started().ns > config_.grace_ns) {
            (void)parent.fail(now,
                              spec.venue != VenueId::Invalid ? "the chosen venue does not quote this listing"
                                                             : "no venue that takes orders quotes this listing");
            return;
        }
        hold(parent, Hold::Stale, now);
        return;
    }
    if (live == nullptr && spec.post_only && !book->post_only) {
        (void)parent.fail(now, "the venue offers no post-only order");
        return;
    }
    const auto instrument = live != nullptr ? live->instrument : book->instrument;
    const auto quote = quote_of(instrument, bids, asks);
    if (quote.state != BookState::Live) {
        hold(parent, quote.state == BookState::Crossed ? Hold::Crossed : Hold::Stale, now);
        return;
    }
    parent.slot().hold = Hold::None;
    const auto scale = market_->scale(instrument);
    if (!scale.has_value()) {
        return;
    }
    const auto tick = scale->tick;
    const auto price = spec.algo == Algo::Pegged
                           ? peg_price(spec.side, spec.peg, quote, tick, spec.limit, spec.post_only)
                           : rest_price(spec.side, spec.limit, quote, tick, spec.limit, spec.post_only);
    if (price.is_undef()) {
        hold(parent, Hold::Stale, now);
        return;
    }

    if (live == nullptr) {
        if (now.ns >= parent.slot().retry_at.ns) {
            place(parent, *book, price, now);
        }
        return;
    }
    if (live->modify_sent.ns != 0) {
        if (now.ns - live->modify_sent.ns >= config_.modify_wait_ns) {
            pull(parent, *live, now);
        }
        return;
    }
    // a late fill left the live child larger than the order
    if (live->leaves().raw > left) {
        pull(parent, *live, now);
        return;
    }
    // no venue id to aim at before the first answer
    if (live->acked.ns == 0) {
        return;
    }
    const auto gap = spec.algo == Algo::Pegged ? spec.peg.min_interval_ns : config_.resend_ns;
    if (now.ns < parent.slot().changed.ns + gap) {
        return;
    }
    if (spec.algo == Algo::Iceberg && spec.iceberg.refill == Refill::OnFill && live->filled.raw > 0 &&
        left > live->leaves().raw) {
        pull(parent, *live, now);
        return;
    }
    const auto threshold = spec.algo == Algo::Pegged ? spec.peg.threshold_ticks : 1U;
    const auto moved = price.raw > live->limit.raw ? price.raw - live->limit.raw : live->limit.raw - price.raw;
    if (moved == 0 || moved < tick.raw * static_cast<std::int64_t>(threshold)) {
        return;
    }
    // only an untouched child: venues read an amend's size as the total or as what is left
    if (book->amend && !parent.slot().no_modify && live->filled.raw == 0 &&
        sink_->modify(live->id, instrument, price)) {
        parent.modify_sent(now, live->id, price);
        parent.slot().changed = now;
        return;
    }
    pull(parent, *live, now);
}

void Desk::place(Parent& parent, const VenueBook& book, FixedE9 price, UnixNanos now) {
    const auto& spec = parent.spec();
    const auto lot = book.lot;
    auto& slot = parent.slot();
    const auto left_lots = lots_in(FixedE9::from_raw(parent.qty().raw - parent.filled().raw), lot);
    const auto want = spec.algo == Algo::Iceberg ? next_show(spec.iceberg, lot, left_lots, slot.random) : left_lots;
    const auto lots = fit_lots(want, left_lots, lot, price, book.min_notional, spec.max_child_notional);
    if (lots == 0) {
        const auto per_lot = value_of(lot, price);
        if (per_lot.raw > spec.max_child_notional.raw) {
            (void)parent.fail(now, "one lot on the venue is worth more than max_child_notional");
        } else if (parent.filled().raw == 0) {
            (void)parent.fail(now, "the order is below the venue's minimum notional");
        } else {
            (void)parent.finish(now);
        }
        return;
    }
    const auto tif = spec.post_only ? ChildTif::PostOnly : ChildTif::Gtc;
    const auto sent = sink_->submit({.instrument = book.instrument,
                                     .side = spec.side,
                                     .lots = lots,
                                     .limit = price,
                                     .tif = tif,
                                     .book = spec.book,
                                     .origin = internal::origin_of(parent),
                                     .repeat_key = internal::repeat_key(parent)});
    if (!sent.has_value()) {
        if (sent.error() == SinkError::Risk && parent.refused() >= config_.max_refusals) {
            (void)parent.fail(now,
                              "the risk gate refused " + std::to_string(config_.max_refusals) + " children in a row");
            return;
        }
        slot.retry_at = UnixNanos{now.ns + config_.resend_ns};
        return;
    }
    const bool first = parent.child_count() == 0;
    parent.add({.id = *sent,
                .instrument = book.instrument,
                .lot = lot,
                .lots = lots,
                .limit = price,
                .sent = now,
                .tif = tif});
    log_child(parent, parent.children().back());
    parent.accepted();
    slot.changed = now;
    const auto why = first ? RepriceWhy::Place : spec.algo == Algo::Iceberg ? RepriceWhy::Refill : RepriceWhy::Peg;
    parent.trace().reprice({.ts = now,
                            .child_id = *sent,
                            .instrument = book.instrument,
                            .price = price,
                            .qty = lot.scaled_by(lots),
                            .why = why});
}

void Desk::hold(Parent& parent, Hold why, UnixNanos now) {
    auto& slot = parent.slot();
    const auto* live = parent.live();
    // an iceberg's price does not follow the book, so a bad book only stops its next child
    if (live != nullptr && parent.spec().algo == Algo::Pegged) {
        pull(parent, *live, now);
    }
    if (slot.hold == why) {
        return;
    }
    slot.hold = why;
    parent.trace().reprice({.ts = now,
                            .child_id = live != nullptr ? live->id : 0,
                            .instrument = live != nullptr ? live->instrument : InstrumentId{},
                            .price = FixedE9::undef(),
                            .qty = FixedE9::from_raw(0),
                            .why = why == Hold::Crossed ? RepriceWhy::Crossed : RepriceWhy::Stale});
}

void Desk::pull(Parent& parent, const Child& child, UnixNanos now) {
    if (child.cancel_sent.ns != 0) {
        return;
    }
    const auto id = child.id;
    sink_->cancel(id);
    parent.cancel_sent(now, id);
}

void Desk::chase_cancels(Parent& parent, UnixNanos now) {
    for (const auto& child : parent.children()) {
        if (child.open && child.cancel_sent.ns != 0 && now.ns - child.cancel_sent.ns >= config_.cancel_retry_ns) {
            sink_->cancel(child.id);
            parent.cancel_sent(now, child.id);
        }
    }
}

void Desk::cancel_children(Parent& parent) {
    const auto now = clock_->wall();
    for (const auto& child : parent.children()) {
        if (child.open) {
            pull(parent, child, now);
        }
    }
}

Parent* Desk::owner_of(std::uint64_t child_id) noexcept {
    const auto it = std::ranges::find_if(parents_, [child_id](const Parent& p) { return p.owns(child_id); });
    return it == parents_.end() ? nullptr : &*it;
}

void Desk::on_fill(
    std::uint64_t child_id, InstrumentId instrument, QtyLots qty, PriceTicks price, FixedE9 fee, QtyLots leaves) {
    const LogChanges logged{*this};
    const auto scale = market_->scale(instrument);
    if (!scale.has_value() || qty.is_undef() || price.is_undef()) {
        return;
    }
    const auto base = scale->lot.scaled_by(qty.raw);
    const auto px = scale->tick.scaled_by(price.raw);
    const auto left = leaves.is_undef() ? FixedE9::undef() : scale->lot.scaled_by(leaves.raw);
    const auto now = clock_->wall();
    auto* parent = owner_of(child_id);
    if (parent == nullptr) {
        return;
    }
    const auto* held = parent->child(child_id);
    const bool was_open = held != nullptr && held->open;
    if (!parent->fill(now, child_id, base, px, fee, left) || !resting(parent->spec().algo)) {
        return;
    }
    if (base.raw > 0) {
        parent->slot().rejects = 0;
    }
    if (const auto* child = parent->child(child_id); was_open && !child->open) {
        parent->trace().reprice({.ts = now,
                                 .child_id = child_id,
                                 .instrument = child->instrument,
                                 .price = FixedE9::undef(),
                                 .qty = FixedE9::from_raw(0),
                                 .why = RepriceWhy::Closed});
    }
    if (parent->status() == Status::Working) {
        rest(*parent, now);
    }
}

void Desk::on_ack(std::uint64_t child_id) {
    const LogChanges logged{*this};
    const auto now = clock_->wall();
    auto* parent = owner_of(child_id);
    if (parent == nullptr) {
        return;
    }
    (void)parent->ack(now, child_id);
    // some venues answer an amend with a fresh ack
    on_modified(child_id);
}

void Desk::on_closed(std::uint64_t child_id, ChildEnd end) {
    const LogChanges logged{*this};
    const auto now = clock_->wall();
    auto* parent = owner_of(child_id);
    if (parent == nullptr) {
        return;
    }
    const auto* child = parent->child(child_id);
    if (child == nullptr || !child->open) {
        (void)parent->close(now, child_id, end);
        return;
    }
    const bool asked = child->cancel_sent.ns != 0;
    const bool untouched = child->filled.raw == 0;
    const auto tif = child->tif;
    const auto instrument = child->instrument;
    // venues answer a post-only that would cross with any of the three
    const bool bounced = tif == ChildTif::PostOnly && !asked && untouched &&
                         (end == ChildEnd::Expired || end == ChildEnd::Cancelled);
    const auto how = bounced ? ChildEnd::Rejected : end;
    (void)parent->close(now, child_id, how);
    if (tif == ChildTif::Ioc) {
        return;
    }
    auto& slot = parent->slot();
    const bool turned_away = how != ChildEnd::Filled && !asked && untouched;
    parent->trace().reprice({.ts = now,
                             .child_id = child_id,
                             .instrument = instrument,
                             .price = FixedE9::undef(),
                             .qty = FixedE9::from_raw(0),
                             .why = turned_away ? RepriceWhy::Rejected : RepriceWhy::Closed});
    if (turned_away) {
        const auto& spec = parent->spec();
        slot.retry_at = UnixNanos{now.ns + (spec.algo == Algo::Pegged ? spec.peg.min_interval_ns : config_.resend_ns)};
        if (++slot.rejects >= config_.max_rejects) {
            (void)parent->fail(now,
                               "the venue turned away " + std::to_string(slot.rejects) + " resting children in a row");
            return;
        }
    } else {
        slot.rejects = 0;
    }
    if (parent->status() == Status::Working) {
        rest(*parent, now);
    }
}

void Desk::on_modified(std::uint64_t child_id) {
    const LogChanges logged{*this};
    const auto now = clock_->wall();
    auto* parent = owner_of(child_id);
    if (parent == nullptr || !parent->modified(now, child_id)) {
        return;
    }
    parent->slot().rejects = 0;
    // venues differ on an amend's size, so one that crossed a fill may be larger than joe thinks
    if (const auto* child = parent->child(child_id); child != nullptr && child->filled.raw > 0) {
        pull(*parent, *child, now);
    }
}

void Desk::on_modify_refused(std::uint64_t child_id) {
    auto* parent = owner_of(child_id);
    if (parent != nullptr && parent->modify_refused(child_id)) {
        parent->slot().no_modify = true;
    }
}

void Desk::on_cancel_refused(std::uint64_t child_id) {
    if (auto* parent = owner_of(child_id); parent != nullptr) {
        parent->cancel_refused(child_id);
    }
}

void Desk::on_trade(InstrumentId instrument, PriceTicks price, QtyLots qty) {
    const auto live = std::ranges::any_of(parents_, [](const Parent& p) { return p.status() == Status::Working; });
    if (!live || qty.is_undef() || price.is_undef()) {
        return;
    }
    const auto scale = market_->scale(instrument);
    if (!scale.has_value()) {
        return;
    }
    const auto base = scale->lot.scaled_by(qty.raw);
    const auto px = scale->tick.scaled_by(price.raw);
    for (auto& parent : parents_) {
        const auto& listing = parent.spec().listing;
        if (parent.status() == Status::Working && listing.symbol == scale->listing.symbol &&
            listing.market == scale->listing.market) {
            parent.market_trade(base, px);
        }
    }
}

void Desk::halt() {
    const LogChanges logged{*this};
    halting_ = true;
    const auto now = clock_->wall();
    for (auto& parent : parents_) {
        if (parent.status() == Status::Working) {
            (void)parent.pause(now);
            cancel_children(parent);
        }
    }
}

void Desk::prune() {
    while (parents_.size() > config_.keep) {
        // one with a child still out keeps its place, or that child's fills go nowhere
        const auto it = std::ranges::find_if(
            parents_, [](const Parent& p) { return terminal(p.status()) && p.live() == nullptr; });
        if (it == parents_.end()) {
            return;
        }
        logged_.erase(it->id());
        parents_.erase(it);
    }
}

} // namespace raikiri::joe
