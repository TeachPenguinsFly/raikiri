#include <raikiri/clientapi/application/book.hpp>
#include <raikiri/common/fixed_point.hpp>

#include <algorithm>
#include <charconv>

namespace raikiri::clientapi {

namespace {

std::string decimal_text(FixedE9 value) {
    char buf[40];
    return std::string{format_e9(value, buf, sizeof(buf))};
}

std::uint64_t digits_of(std::string_view text) noexcept {
    std::uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

std::optional<FixedE9> decimal_of(std::string_view text) noexcept {
    const auto parsed = parse_decimal_e9(text);
    return parsed.has_value() ? std::optional<FixedE9>{*parsed} : std::nullopt;
}

// a venue's average price need not sit on the tick; nearest is as close as the book can hold it
std::int64_t nearest_step(std::int64_t value, std::int64_t step) noexcept {
    return step <= 0 ? 0 : (value + step / 2) / step;
}

std::optional<InstrumentBase> base_of_scale(const InstrumentScale& scale) {
    const auto tick = decimal_of(scale.tick_size);
    const auto lot = decimal_of(scale.lot_size);
    const auto value = decimal_of(scale.contract_value);
    if (!tick.has_value() || !lot.has_value() || !value.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *value;
    base.kind = scale.contract_kind == "inverse" ? ContractKind::Inverse : ContractKind::Linear;
    return base;
}

const char* idem_state_name(IdemState state) noexcept {
    switch (state) {
    case IdemState::Pending:
        return "pending";
    case IdemState::Done:
        return "done";
    case IdemState::Unknown:
        return "unknown";
    }
    return "unknown";
}

IdemState idem_state_of(std::string_view name) noexcept {
    if (name == "pending") {
        return IdemState::Pending;
    }
    return name == "done" ? IdemState::Done : IdemState::Unknown;
}

} // namespace

void ClientBook::load(std::uint64_t now_ns) {
    const std::scoped_lock lock{mutex_};
    log_->replay([this, now_ns](const LedgerRecord& record) {
        if (record.order.has_value()) {
            apply_order(*record.order);
        }
        if (record.parent.has_value()) {
            apply_parent(*record.parent);
        }
        if (record.fill.has_value()) {
            (void)apply_fill(*record.fill);
        }
        if (record.idem.has_value()) {
            apply_idem(*record.idem, now_ns);
        }
    });
}

void ClientBook::apply_order(const OwnedOrder& order) {
    orders_.insert_or_assign(order.id, order);
}

void ClientBook::apply_parent(const OwnedParent& parent) {
    parents_.insert_or_assign(parent.id, parent);
}

void ClientBook::apply_idem(const IdemRecord& record, std::uint64_t now_ns) {
    idempotency_.restore(record.key,
                         {.request_hash = record.request_hash,
                          .state = idem_state_of(record.state),
                          .reply = {.status = record.status, .body = record.body},
                          .at_ns = digits_of(record.at_ns)},
                         now_ns);
    if (record.state == "released") {
        idempotency_.release(record.key);
    }
}

bool ClientBook::apply_fill(const OwnedFill& owned) {
    const auto& fill = owned.fill;
    if (!booked_.emplace(fill.venue, fill.exec_id).second) {
        return false;
    }
    const auto client = fill.client_id.value_or(std::string{});
    fills_[client].push_back(fill);
    const auto ts = digits_of(fill.ts_event);
    last_fill_ns_ = std::max(last_fill_ns_, ts);

    auto base = base_of_scale(owned.scale);
    const auto price = decimal_of(fill.price);
    const auto qty = decimal_of(fill.qty);
    if (!base.has_value() || !price.has_value() || !qty.has_value()) {
        return true;
    }
    auto& held = held_[{client, fill.instrument_id}];
    held.base = *base;
    held.instrument_id = fill.instrument_id;
    held.account = fill.account.value_or(std::string{});
    held.symbol = fill.symbol.value_or(std::string{});
    const auto fee = fill.fee.has_value() ? decimal_of(*fill.fee) : std::nullopt;
    (void)held.position.apply_fill(held.base,
                                   fill.side == "sell" ? Side::Sell : Side::Buy,
                                   QtyLots::from_raw(nearest_step(qty->raw, base->lot_size.raw)),
                                   PriceTicks::from_raw(nearest_step(price->raw, base->tick_size.raw)),
                                   fee.value_or(FixedE9::undef()),
                                   UnixNanos{ts});
    return true;
}

bool ClientBook::record_order(const OwnedOrder& order) {
    const std::scoped_lock lock{mutex_};
    apply_order(order);
    return log_->append({.order = order});
}

bool ClientBook::record_parent(const OwnedParent& parent) {
    const std::scoped_lock lock{mutex_};
    apply_parent(parent);
    return log_->append({.parent = parent});
}

void ClientBook::close_parent(const std::string& id) {
    const std::scoped_lock lock{mutex_};
    const auto found = parents_.find(id);
    if (found == parents_.end() || found->second.closed) {
        return;
    }
    found->second.closed = true;
    (void)log_->append({.parent = found->second});
}

std::optional<OwnedOrder> ClientBook::find_order(const std::string& id) const {
    const std::scoped_lock lock{mutex_};
    const auto found = orders_.find(id);
    return found == orders_.end() ? std::nullopt : std::optional<OwnedOrder>{found->second};
}

void ClientBook::note_event(const OrderEventRow& event) {
    const std::scoped_lock lock{mutex_};
    auto& live = live_[event.client_order_id];
    if (event.event_type == "filled" && event.leaves.has_value()) {
        live.leaves_after_fill = event.leaves;
        const auto left = decimal_of(*event.leaves);
        if (left.has_value() && left->raw == 0) {
            live.closed_as = "filled";
        }
    } else if (event.event_type == "canceled" || event.event_type == "expired" || event.event_type == "rejected" ||
               event.event_type == "denied") {
        live.closed_as = event.event_type;
    }
}

std::optional<LiveOrder> ClientBook::live_order(const std::string& id) const {
    const std::scoped_lock lock{mutex_};
    const auto found = live_.find(id);
    return found == live_.end() ? std::nullopt : std::optional<LiveOrder>{found->second};
}

std::optional<OwnedParent> ClientBook::find_parent(const std::string& id) const {
    const std::scoped_lock lock{mutex_};
    const auto found = parents_.find(id);
    return found == parents_.end() ? std::nullopt : std::optional<OwnedParent>{found->second};
}

std::vector<OwnedParent> ClientBook::open_parents() const {
    const std::scoped_lock lock{mutex_};
    std::vector<OwnedParent> out;
    for (const auto& [id, parent] : parents_) {
        if (!parent.closed) {
            out.push_back(parent);
        }
    }
    return out;
}

std::vector<OwnedParent> ClientBook::parents_of(const std::string& client_id) const {
    const std::scoped_lock lock{mutex_};
    std::vector<OwnedParent> out;
    for (const auto& [id, parent] : parents_) {
        if (parent.client_id == client_id) {
            out.push_back(parent);
        }
    }
    return out;
}

std::optional<PositionRow> ClientBook::record_fill(const OwnedFill& fill) {
    const std::scoped_lock lock{mutex_};
    if (!apply_fill(fill)) {
        return std::nullopt;
    }
    (void)log_->append({.fill = fill});
    const auto client = fill.fill.client_id.value_or(std::string{});
    const auto held = held_.find({client, fill.fill.instrument_id});
    if (held == held_.end()) {
        return PositionRow{.client_id = client,
                           .account = fill.fill.account.value_or(std::string{}),
                           .symbol = fill.fill.symbol.value_or(std::string{}),
                           .instrument_id = fill.fill.instrument_id};
    }
    return row_of(client, held->second);
}

Page<FillRow> ClientBook::fills_of(const std::string& client_id,
                                   const FillFilter& filter,
                                   std::string_view cursor,
                                   std::size_t limit) const {
    const std::scoped_lock lock{mutex_};
    Page<FillRow> page;
    const auto found = fills_.find(client_id);
    if (found == fills_.end()) {
        return page;
    }
    const auto& rows = found->second;
    // the cursor counts rows from the oldest, so a fill booked after it does not shift the next page
    auto at = cursor.empty() ? rows.size() : std::min<std::size_t>(digits_of(cursor), rows.size());
    while (at > 0 && page.items.size() < limit) {
        const auto& row = rows[--at];
        if (!filter.account.empty() && row.account.value_or(std::string{}) != filter.account) {
            continue;
        }
        if (!filter.instrument_id.empty() && row.instrument_id != filter.instrument_id) {
            continue;
        }
        if (filter.since_ns != 0 && digits_of(row.ts_event) < filter.since_ns) {
            continue;
        }
        page.items.push_back(row);
    }
    if (at > 0) {
        page.next_cursor = std::to_string(at);
    }
    return page;
}

PositionRow ClientBook::row_of(const std::string& client_id, const Held& held) const {
    PositionRow row{
        .client_id = client_id, .account = held.account, .symbol = held.symbol, .instrument_id = held.instrument_id};
    const auto units = FixedE9::from_raw(detail::checked_mul(held.position.qty().raw, held.base.lot_size.raw));
    row.qty = decimal_text(units);
    if (const auto avg = held.position.avg_entry_price(held.base); !avg.is_undef()) {
        row.avg_entry_price = decimal_text(avg);
    }
    row.realized = decimal_text(held.position.realized());
    row.fees = decimal_text(held.position.fees());
    row.ts_updated = std::to_string(held.position.ts_updated().ns);
    return row;
}

std::vector<PositionRow> ClientBook::positions_of(const std::string& client_id) const {
    const std::scoped_lock lock{mutex_};
    std::vector<PositionRow> out;
    for (auto at = held_.lower_bound({client_id, std::string{}}); at != held_.end() && at->first.first == client_id;
         ++at) {
        if (!at->second.position.flat()) {
            out.push_back(row_of(client_id, at->second));
        }
    }
    return out;
}

std::uint64_t ClientBook::last_fill_ns() const {
    const std::scoped_lock lock{mutex_};
    return last_fill_ns_;
}

IdemBegin ClientBook::idem_begin(const std::string& key, std::string_view hash, std::uint64_t now_ns, bool& durable) {
    const std::scoped_lock lock{mutex_};
    auto begun = idempotency_.begin(key, hash, now_ns);
    durable = true;
    if (begun.verdict == IdemVerdict::Fresh) {
        durable = log_->append({.idem = IdemRecord{.key = key,
                                                   .request_hash = std::string{hash},
                                                   .state = idem_state_name(IdemState::Pending),
                                                   .at_ns = std::to_string(now_ns)}});
        if (!durable) {
            idempotency_.release(key);
        }
    }
    return begun;
}

void ClientBook::idem_finish(const std::string& key, const StoredReply& reply) {
    const std::scoped_lock lock{mutex_};
    const auto* entry = idempotency_.find(key);
    const auto hash = entry == nullptr ? std::string{} : entry->request_hash;
    const auto at_ns = entry == nullptr ? std::uint64_t{0} : entry->at_ns;
    idempotency_.finish(key, reply);
    (void)log_->append({.idem = IdemRecord{.key = key,
                                           .request_hash = hash,
                                           .state = idem_state_name(IdemState::Done),
                                           .status = reply.status,
                                           .body = reply.body,
                                           .at_ns = std::to_string(at_ns)}});
}

void ClientBook::idem_unknown(const std::string& key) {
    const std::scoped_lock lock{mutex_};
    // the pending record already on disk comes back as unknown, so a restart agrees
    idempotency_.mark_unknown(key);
}

void ClientBook::idem_release(const std::string& key) {
    const std::scoped_lock lock{mutex_};
    idempotency_.release(key);
    (void)log_->append({.idem = IdemRecord{.key = key, .state = "released"}});
}

} // namespace raikiri::clientapi
