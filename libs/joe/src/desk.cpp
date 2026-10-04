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

[[nodiscard]] Refusal refusal(TransitionError error) {
    return {.error = DeskError::Conflict, .detail = std::string{to_string(error)}};
}

} // namespace internal

Desk::Desk(
    const MarketView& market, OrderSink& sink, const Clock& clock, const VolumeProfile& recorded, DeskConfig config)
    : market_{&market}, sink_{&sink}, clock_{&clock}, recorded_{&recorded}, config_{config} {}

std::expected<std::uint64_t, Refusal> Desk::start(ParentSpec spec) {
    if (const auto valid = validate(spec); !valid.has_value()) {
        return std::unexpected{Refusal{.error = DeskError::BadSpec, .detail = std::string{to_string(valid.error())}}};
    }
    if (!sink_->running()) {
        return std::unexpected{Refusal{.error = DeskError::NotRunning, .detail = "joe is not running in strategyd"}};
    }
    const auto now = clock_->wall();
    auto& parent = parents_.emplace_back(next_id_++, spec, now);
    const auto id = parent.id();
    begin(parent, now);
    prune();
    return id;
}

std::expected<void, Refusal> Desk::cancel(std::uint64_t id) {
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

Parent* Desk::find_mut(std::uint64_t id) noexcept {
    const auto it = std::ranges::find_if(parents_, [id](const Parent& p) { return p.id() == id; });
    return it == parents_.end() ? nullptr : &*it;
}

void Desk::tick() {
    if (!sink_->running()) {
        return;
    }
    const auto now = clock_->wall();
    for (auto& parent : parents_) {
        if (parent.status() == Status::Pending) {
            begin(parent, now);
        } else if (parent.status() == Status::Working) {
            work(parent, now);
        }
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
    }
    work(parent, now);
}

void Desk::work(Parent& parent, UnixNanos now) {
    const auto& spec = parent.spec();
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
        const auto sent = sink_->submit(
            {.instrument = leg.instrument, .side = spec.side, .lots = lots, .limit = leg.limit});
        if (sent.has_value()) {
            parent.add({.id = *sent,
                        .instrument = leg.instrument,
                        .lot = leg.lot,
                        .lots = lots,
                        .limit = leg.limit,
                        .sent = now});
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

void Desk::cancel_children(Parent& parent) {
    for (const auto& child : parent.children()) {
        if (child.open) {
            sink_->cancel(child.id);
        }
    }
}

void Desk::on_fill(std::uint64_t child_id, InstrumentId instrument, QtyLots qty, PriceTicks price, FixedE9 fee) {
    const auto scale = market_->scale(instrument);
    if (!scale.has_value() || qty.is_undef() || price.is_undef()) {
        return;
    }
    const auto base = scale->lot.scaled_by(qty.raw);
    const auto px = scale->tick.scaled_by(price.raw);
    for (auto& parent : parents_) {
        if (parent.fill(child_id, base, px, fee)) {
            return;
        }
    }
}

void Desk::on_closed(std::uint64_t child_id) {
    for (auto& parent : parents_) {
        if (parent.close(child_id)) {
            return;
        }
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
        const auto it = std::ranges::find_if(parents_, [](const Parent& p) { return terminal(p.status()); });
        if (it == parents_.end()) {
            return;
        }
        parents_.erase(it);
    }
}

} // namespace raikiri::joe
