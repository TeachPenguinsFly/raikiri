#include <raikiri/clientapi/application/feed.hpp>

#include <algorithm>

namespace raikiri::clientapi {

namespace {

constexpr std::uint64_t k_ms = 1'000'000ULL;
constexpr std::uint64_t k_parked_ns = 10'000 * k_ms;
constexpr std::size_t k_parked_max = 8'192;
constexpr std::uint64_t k_algos_ns = 1'000 * k_ms;
constexpr std::uint64_t k_algos_urgent_ns = 200 * k_ms;
constexpr std::uint64_t k_parked_retry_ns = 20 * k_ms;
constexpr std::uint64_t k_balances_ns = 2'000 * k_ms;
constexpr std::uint64_t k_fills_ns = 5'000 * k_ms;
constexpr std::uint64_t k_fills_overlap_ns = 60'000 * k_ms;
constexpr std::uint64_t k_first_catchup_ns = 86'400'000 * k_ms;
constexpr int k_fill_pages = 10;

} // namespace

PrivateFeed::PrivateFeed(
    Backend& backend, ClientBook& book, StreamHub& hub, InstrumentDirectory& instruments, WallClock wall)
    : backend_{&backend}, book_{&book}, hub_{&hub}, instruments_{&instruments}, wall_{std::move(wall)} {
    const auto last = book.last_fill_ns();
    const auto now = wall_().ns;
    fills_since_ns_ = last != 0 ? last : now - std::min(now, k_first_catchup_ns);
}

void PrivateFeed::park(Parked event) {
    if (parked_.size() >= k_parked_max) {
        parked_.pop_front();
    }
    const auto now = wall_().ns;
    parked_.push_back({.event = std::move(event), .expires_ns = now + k_parked_ns});
    // a child stays unknown until its parent is read, so read it soon
    next_algos_ns_ = std::min(next_algos_ns_, now + k_algos_urgent_ns);
}

bool PrivateFeed::deliver(const OrderEventRow& event) {
    const auto owned = book_->find_order(event.client_order_id);
    if (!owned.has_value()) {
        return false;
    }
    book_->note_event(event);
    auto row = event;
    row.client_id = owned->client_id;
    row.account = owned->account;
    row.symbol = owned->symbol;
    row.parent_id = owned->parent_id;
    hub_->publish(owned->client_id, mask_of(Channel::Orders), owned->account, owned->symbol, std::move(row));
    return true;
}

bool PrivateFeed::deliver(const FillRow& fill) {
    if (!fill.client_order_id.has_value()) {
        return true; // a fill with no order of ours belongs to no client
    }
    const auto owned = book_->find_order(*fill.client_order_id);
    if (!owned.has_value()) {
        return false;
    }
    const auto listing = instruments_->lookup(fill.instrument_id);
    if (!listing.has_value()) {
        return false;
    }
    auto row = fill;
    row.client_id = owned->client_id;
    row.account = owned->account;
    row.symbol = owned->symbol;
    row.parent_id = owned->parent_id;
    const bool crossed = row.liquidity == "internal";
    if (crossed) {
        row.internal = true;
    }
    const auto position = book_->record_fill({.fill = row,
                                              .scale = {.tick_size = listing->tick_size,
                                                        .lot_size = listing->lot_size,
                                                        .contract_value = listing->contract_value,
                                                        .contract_kind = listing->contract_kind}});
    if (!position.has_value()) {
        return true; // booked before, by the other path
    }
    hub_->publish(owned->client_id, mask_of(Channel::Fills), owned->account, owned->symbol, std::move(row));
    hub_->publish(owned->client_id, mask_of(Channel::Positions), owned->account, owned->symbol, *position);
    if (crossed) {
        // no gateway saw this, so the live feed never says what it did to the order
        const auto order = backend_->order(*fill.client_order_id);
        (void)deliver(OrderEventRow{.client_order_id = *fill.client_order_id,
                                    .instrument_id = fill.instrument_id,
                                    .event_type = "filled",
                                    .price = fill.price,
                                    .qty = fill.qty,
                                    .leaves = order.has_value() ? std::optional{order->leaves} : std::nullopt,
                                    .fee = fill.fee,
                                    .exec_id = fill.exec_id,
                                    .ts_event = fill.ts_event});
    }
    return true;
}

// an event can outrun the worker recording its order, and a child stays unknown until its parent is polled
void PrivateFeed::on_order_event(OrderEventRow event) {
    if (!deliver(event)) {
        park(std::move(event));
    }
}

void PrivateFeed::on_fill(FillRow fill) {
    if (!deliver(fill)) {
        park(std::move(fill));
    }
}

void PrivateFeed::on_gap(std::string_view reason) {
    hub_->publish_connected(0, {}, FeedGap{.reason = std::string{reason}});
    // what the ring lost is still in postgres; fills come back through the next pull
    next_fills_ns_ = 0;
}

void PrivateFeed::retry_parked(std::uint64_t now_ns) {
    const auto rounds = parked_.size();
    for (std::size_t i = 0; i < rounds; ++i) {
        auto waiting = std::move(parked_.front());
        parked_.pop_front();
        const auto done = std::visit([this](const auto& event) { return deliver(event); }, waiting.event);
        // what nobody claims in time was never a client's: a strategy's order, or the terminal's
        if (!done && waiting.expires_ns > now_ns) {
            parked_.push_back(std::move(waiting));
        }
    }
}

void PrivateFeed::poll_algos() {
    for (const auto& owned : book_->open_parents()) {
        const auto row = backend_->parent(owned.id);
        if (!row.has_value() && row.error().failure != BackendFailure::Answered) {
            continue; // the backend is away, which says nothing about this parent
        }
        const auto lost = (!row.has_value() && row.error().status == 404) ||
                          (row.has_value() && !owned.created.empty() && row->created != owned.created);
        if (lost) {
            hub_->publish(owned.client_id,
                          mask_of(Channel::Algos),
                          {},
                          owned.symbol,
                          AlgoLost{.id = owned.id, .reason = "strategyd restarted and forgot this parent"});
            book_->close_parent(owned.id);
            algos_.erase(owned.id);
            continue;
        }
        if (!row.has_value()) {
            continue;
        }
        for (const auto& child : row->children.value_or(std::vector<ChildRow>{})) {
            if (book_->find_order(child.client_order_id).has_value()) {
                continue;
            }
            const auto listing = instruments_->lookup(child.instrument_id);
            if (!listing.has_value()) {
                continue;
            }
            (void)book_->record_order(OwnedOrder{.id = child.client_order_id,
                                                 .client_id = owned.client_id,
                                                 .key_id = owned.key_id,
                                                 .account = listing->account,
                                                 .symbol = listing->local_symbol,
                                                 .instrument_id = child.instrument_id,
                                                 .side = row->side,
                                                 .order_type = "limit",
                                                 .tif = "ioc",
                                                 .price = child.limit,
                                                 .qty = child.qty,
                                                 .parent_id = owned.id,
                                                 .ts = child.sent});
        }
        auto& seen = algos_[owned.id];
        if (seen.status != row->status || seen.filled != row->filled || seen.working != row->working ||
            seen.slices_fired != row->slices_fired) {
            seen = {.status = row->status,
                    .filled = row->filled,
                    .working = row->working,
                    .slices_fired = row->slices_fired};
            auto published = *row;
            published.client_id = owned.client_id;
            published.children.reset();
            hub_->publish(owned.client_id, mask_of(Channel::Algos), {}, owned.symbol, std::move(published));
        }
        if (parent_is_terminal(row->status)) {
            book_->close_parent(owned.id);
            algos_.erase(owned.id);
        }
    }
}

void PrivateFeed::poll_balances() {
    if (!hub_->any_connected()) {
        return;
    }
    const auto rows = backend_->balances();
    if (!rows.has_value()) {
        return;
    }
    for (const auto& row : *rows) {
        auto& last = balances_[row.account];
        if (last != row.assets) {
            last = row.assets;
            hub_->publish_connected(mask_of(Channel::Balances), row.account, row);
        }
    }
}

void PrivateFeed::poll_fills(std::uint64_t now_ns) {
    const auto since = std::to_string(fills_since_ns_ - std::min(fills_since_ns_, k_fills_overlap_ns));
    for (int page = 0; page < k_fill_pages; ++page) {
        const auto rows = backend_->fills(since, fills_cursor_);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& fill : rows->items) {
            (void)deliver(fill);
        }
        const auto& next = rows->next_cursor;
        if (!next.has_value()) {
            fills_cursor_.clear();
            fills_since_ns_ = now_ns;
            return;
        }
        // a long catch-up goes on from here next tick, with the same lower bound
        fills_cursor_ = *next;
    }
}

std::uint64_t PrivateFeed::tick() {
    const auto now = wall_().ns;
    if (now >= next_algos_ns_) {
        poll_algos();
        next_algos_ns_ = now + (parked_.empty() ? k_algos_ns : k_algos_urgent_ns);
    }
    retry_parked(now);
    if (now >= next_balances_ns_) {
        poll_balances();
        next_balances_ns_ = now + k_balances_ns;
    }
    if (now >= next_fills_ns_) {
        poll_fills(now);
        next_fills_ns_ = now + k_fills_ns;
    }
    return parked_.empty() ? k_algos_ns : k_parked_retry_ns;
}

} // namespace raikiri::clientapi
