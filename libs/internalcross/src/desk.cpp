#include <raikiri/internalcross/application/desk.hpp>
#include <raikiri/oms/notional.hpp>
#include <raikiri/portfolio/pnl.hpp>

#include <fmt/format.h>

#include <algorithm>

namespace raikiri::internalcross {

CrossDesk::CrossDesk(CrossKernel& kernel,
                     CrossLog& log,
                     oms::OrderStore& store,
                     oms::ExecPath& exec,
                     portfolio::PositionBook& positions,
                     oms::OrderTrail& trail,
                     audit::Sink& audit)
    : kernel_{&kernel}, log_{&log}, store_{&store}, exec_{&exec}, positions_{&positions}, trail_{&trail},
      audit_{&audit}, leg_ids_{0, 0xC505ULL} {
    held_.reserve(k_max_held);
    opposite_.reserve(k_max_held);
}

void CrossDesk::configure(const Settings& settings) {
    settings_ = settings;
    quotes_ = {.max_quote_age_ns = settings.max_quote_age_ms * 1'000'000ULL, .max_spread_bp = settings.max_spread_bp};
    books_.clear();
}

const BookPolicy* CrossDesk::policy_of(std::uint16_t book) {
    if (const auto it = books_.find(book); it != books_.end()) {
        return it->second.policy;
    }
    const auto kind = kernel_->kind_of(book);
    // a book opened elsewhere may not have reached this process's registry yet, so this is not its last word
    if (kind == BookKind::Unknown) {
        return nullptr;
    }
    BookEntry entry{.name = kernel_->name_of(book)};
    if (kind == BookKind::Client) {
        const auto found = settings_.books.find(entry.name);
        entry.policy = found != settings_.books.end() ? &found->second : nullptr;
    }
    return books_.emplace(book, std::move(entry)).first->second.policy;
}

bool CrossDesk::wants(std::uint16_t book, const Instrument& listed) {
    if (!settings_.enabled) {
        return false;
    }
    const auto* policy = policy_of(book);
    const auto& base = base_of(listed);
    return policy != nullptr && policy->enabled && allows(*policy, base.local_symbol.view(), base.id.market());
}

bool CrossDesk::holds(ClientOrderId id) const noexcept {
    return std::ranges::any_of(held_, [id](const Held& held) { return held.interest.order == id; });
}

void CrossDesk::forget(ClientOrderId id) noexcept {
    std::erase_if(held_, [id](const Held& held) { return held.interest.order == id; });
}

std::span<const ClientOrderId> CrossDesk::take_due(UnixNanos now) noexcept {
    std::size_t count = 0;
    for (auto it = held_.begin(); it != held_.end() && count < due_.size();) {
        if (it->until_ns <= now.ns) {
            due_[count++] = it->interest.order;
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    return std::span<const ClientOrderId>{due_}.first(count);
}

void CrossDesk::settle_held(ClientOrderId id, QtyLots qty) noexcept {
    const auto it = std::ranges::find_if(held_, [id](const Held& held) { return held.interest.order == id; });
    if (it == held_.end()) {
        return;
    }
    it->interest.leaves = QtyLots::from_raw(it->interest.leaves.raw - qty.raw);
    if (it->interest.leaves.raw <= 0) {
        held_.erase(it);
    }
}

void CrossDesk::count(std::optional<Refusal> why) noexcept {
    if (why.has_value()) {
        ++stats_.refused[static_cast<std::size_t>(*why)];
    }
}

void CrossDesk::add(const oms::CrossRecord& record) {
    ++stats_.crosses;
    ++(record.buy.desk || record.sell.desk ? stats_.against_desk : stats_.against_clients);
    stats_.notional = stats_.notional + record.notional;
    stats_.saved = stats_.saved + record.saved;
    stats_.fees = stats_.fees + record.buy.fee + record.sell.fee;
    for (const auto* leg : {&record.buy, &record.sell}) {
        auto& by_book = stats_.notional_by_book[leg->order.strategy_id()];
        by_book = by_book + record.notional;
    }
}

void CrossDesk::restore(const oms::CrossRecord& record) {
    add(record);
}

void CrossDesk::legs_start_after(std::uint64_t seq) noexcept {
    leg_seq_ = std::max(leg_seq_, seq);
}

bool CrossDesk::room_for(std::uint16_t a, std::uint16_t b, InstrumentId instrument) const noexcept {
    const auto missing = (positions_->find({.strategy_id = a, .instrument = instrument}) == nullptr ? 1U : 0U) +
                         (positions_->find({.strategy_id = b, .instrument = instrument}) == nullptr ? 1U : 0U);
    return positions_->size() + missing <= positions_->capacity() && store_->open_count() < store_->capacity();
}

std::optional<ClientOrderId>
CrossDesk::open_desk_leg(std::uint16_t book, const OrderCmd& against, QtyLots qty, PriceTicks price, UnixNanos now) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = against.side == Side::Buy ? Side::Sell : Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Ioc;
    cmd.instrument = against.instrument;
    cmd.client_order_id = ClientOrderId::make_internal_leg(book, ++leg_seq_);
    cmd.exchange_client_order_id = leg_ids_.next_exchange_client_id(now);
    cmd.price = price;
    cmd.qty = qty;
    cmd.ts_created = now;
    if (!store_->create(cmd, cmd.exchange_client_order_id).has_value()) {
        return std::nullopt;
    }
    trail_->command("order.created", cmd, now);
    log_->order(cmd, {.parent = against.client_order_id.raw});
    OrderEvent submitted{};
    submitted.type = OrderEventType::Submitted;
    submitted.side = cmd.side;
    submitted.instrument = cmd.instrument;
    submitted.client_order_id = cmd.client_order_id;
    submitted.qty = qty;
    submitted.leaves_qty = qty;
    submitted.price = price;
    submitted.ts_init = now;
    (void)store_->apply_event(submitted);
    return cmd.client_order_id;
}

bool CrossDesk::book_cross(ClientOrderId arriving,
                           ClientOrderId counter,
                           bool counter_is_desk,
                           const Allocation& allocation,
                           std::int64_t arriving_fee_bp,
                           const Reference& ref,
                           const Instrument& listed,
                           UnixNanos now) {
    const auto* first = store_->find(arriving);
    const auto* second = store_->find(counter);
    if (first == nullptr || second == nullptr || first->leaves().raw < allocation.qty.raw ||
        second->leaves().raw < allocation.qty.raw || first->side() == second->side()) {
        return false;
    }
    const bool arriving_buys = first->side() == Side::Buy;
    const auto* buyer = arriving_buys ? first : second;
    const auto* seller = arriving_buys ? second : first;
    const auto& base = base_of(listed);
    const auto valued = oms::notional(listed, allocation.price, allocation.qty);
    const auto notional = valued.has_value() ? oms::to_e9(*valued) : FixedE9::from_raw(0);
    const auto buy_fee_bp = arriving_buys ? arriving_fee_bp : allocation.counter_fee_bp;
    const auto sell_fee_bp = arriving_buys ? allocation.counter_fee_bp : arriving_fee_bp;
    if (!valued.has_value() && (buy_fee_bp != 0 || sell_fee_bp != 0)) {
        count(Refusal::FeeUnpriced);
        return false;
    }
    oms::CrossRecord record{};
    record.instrument = base.id;
    record.symbol = base.exchange_symbol;
    record.price = allocation.price;
    record.qty = allocation.qty;
    record.ref_bid = ref.bid;
    record.ref_ask = ref.ask;
    record.notional = notional;
    record.saved = venue_cost_saved(ref, notional, base.taker_fee);
    record.ts = now;
    record.buy = {.order = buyer->client_order_id(),
                  .leaves_after = QtyLots::from_raw(buyer->leaves().raw - allocation.qty.raw),
                  .fee = fee_of(notional, buy_fee_bp),
                  .desk = counter_is_desk && !arriving_buys};
    record.sell = {.order = seller->client_order_id(),
                   .leaves_after = QtyLots::from_raw(seller->leaves().raw - allocation.qty.raw),
                   .fee = fee_of(notional, sell_fee_bp),
                   .desk = counter_is_desk && arriving_buys};

    const auto apply_leg = [&](Side side, oms::CrossLeg& leg, const oms::Order& order, BookedLeg& out) {
        auto event = oms::cross_leg_event(record, side);
        event.order_type = order.order_type();
        event.tif = order.tif();
        event.flags = order.flags();
        event.exchange_client_order_id = order.exchange_client_order_id();
        const auto done = exec_->apply(event, false, now);
        trail_->event(event, done.order, now);
        const auto applied = positions_->on_fill(listed, event, done.delta);
        out.event = event;
        out.applied = applied.has_value() ? *applied : portfolio::Position::Applied{};
        out.position = positions_->find({.strategy_id = leg.order.strategy_id(), .instrument = record.instrument});
        if (out.position != nullptr) {
            leg.net_qty = out.position->qty();
            leg.cost = out.position->cost();
            leg.realized = out.position->realized();
            leg.fees = out.position->fees();
            leg.fill_count = out.position->fill_count();
            leg.ts_opened = out.position->ts_opened();
        }
    };

    // the order objects move on with their events, and a leg's position must be in the record before the frame
    const auto buyer_order = *buyer;
    const auto seller_order = *seller;
    BookedLeg buy{};
    BookedLeg sell{};
    apply_leg(Side::Buy, record.buy, buyer_order, buy);
    apply_leg(Side::Sell, record.sell, seller_order, sell);
    log_->crossed(record, buy, sell, listed);
    audit_cross(record, base.exchange_symbol.view(), now);
    add(record);
    kernel_->booked(buy.event, !record.buy.desk);
    kernel_->booked(sell.event, !record.sell.desk);
    return true;
}

void CrossDesk::audit_cross(const oms::CrossRecord& record, std::string_view symbol, UnixNanos now) noexcept {
    std::array<char, 32> bid{};
    std::array<char, 32> ask{};
    std::array<char, 32> saved{};
    std::array<char, 512> detail{};
    const auto wrote = fmt::format_to_n(detail.data(),
                                        detail.size(),
                                        R"({{"buy_order":"{}","sell_order":"{}","buy_book":"{}","sell_book":"{}",)"
                                        R"("desk_leg":{},"instrument":"{}","symbol":"{}","price_ticks":"{}",)"
                                        R"("qty_lots":"{}","ref_bid":"{}","ref_ask":"{}","saved":"{}"}})",
                                        record.buy.order.raw,
                                        record.sell.order.raw,
                                        record.buy.order.strategy_id(),
                                        record.sell.order.strategy_id(),
                                        record.buy.desk || record.sell.desk,
                                        record.instrument.packed,
                                        symbol,
                                        record.price.raw,
                                        record.qty.raw,
                                        std::string_view{format_e9(record.ref_bid, bid.data(), bid.size())},
                                        std::string_view{format_e9(record.ref_ask, ask.data(), ask.size())},
                                        std::string_view{format_e9(record.saved, saved.data(), saved.size())});
    for (const auto* leg : {&record.buy, &record.sell}) {
        std::array<char, 40> subject{};
        const auto about = fmt::format_to_n(subject.data(), subject.size(), "order:{}", leg->order.raw);
        audit_->append({.ts = now,
                        .actor = "desk:internal",
                        .action = "order.crossed",
                        .subject = {subject.data(), about.size},
                        .detail_json = {detail.data(), std::min(wrote.size, detail.size())}});
    }
}

Arrived CrossDesk::on_arrival(ClientOrderId id, const Instrument& listed, UnixNanos now) {
    Arrived out{};
    const auto* order = store_->find(id);
    const auto* policy = policy_of(id.strategy_id());
    if (order == nullptr || policy == nullptr) {
        return out;
    }
    const auto instrument = order->instrument();
    const auto refused = [&](Refusal why) {
        out.why = why;
        count(why);
        return out;
    };
    if (has_flag(order->flags(), OrderFlags::PostOnly)) {
        return refused(Refusal::PostOnly); // it asked to rest at the venue as a maker
    }
    if (policy->fee_bp != 0 && portfolio::settle_side(listed) == portfolio::SettleSide::Base) {
        return refused(Refusal::FeeUnpriced); // the book's sums are in the coin and the fee would be in quote
    }
    std::array<Touch, k_max_touches> touches{};
    const auto seen = kernel_->touches(instrument, touches);
    const auto ref = best_touch(std::span{touches}.first(seen), quotes_, now);
    if (!ref.has_value()) {
        return refused(ref.error());
    }

    const bool spot = instrument.market() == MarketType::Spot;
    const auto cap_of = [&](std::uint16_t book, Side side, ClientOrderId self) {
        if (!spot || side != Side::Sell) {
            return QtyLots::undef();
        }
        const auto held = kernel_->facts(book, instrument, self);
        return QtyLots::from_raw(std::max<std::int64_t>(0, held.position.raw - held.working_sell.raw));
    };

    const Interest arriving{.order = id,
                            .book = id.strategy_id(),
                            .side = order->side(),
                            .limit = order->order_type() == OrderType::Market ? PriceTicks::undef() : order->price(),
                            .leaves = order->leaves(),
                            .fee_bp = policy->fee_bp,
                            .sell_cap = cap_of(id.strategy_id(), order->side(), id)};
    const auto counter_side = order->side() == Side::Buy ? Side::Sell : Side::Buy;
    opposite_.clear();
    for (const auto& held : held_) {
        if (held.instrument != instrument || held.interest.side != counter_side) {
            continue;
        }
        auto interest = held.interest;
        interest.sell_cap = cap_of(interest.book, interest.side, interest.order);
        opposite_.push_back(interest);
    }
    std::optional<Inventory> desk{};
    if (const auto hedge = settings_.against_desk ? kernel_->hedge_book() : std::nullopt; hedge.has_value()) {
        const auto facts = kernel_->facts(*hedge, instrument, ClientOrderId{});
        desk = Inventory{.book = *hedge,
                         .position = facts.position,
                         .working_buy = facts.working_buy,
                         .working_sell = facts.working_sell};
    }
    OrderCmd against{};
    against.kind = CmdKind::Create;
    against.side = order->side();
    against.order_type = order->order_type();
    against.tif = order->tif();
    against.instrument = instrument;
    against.client_order_id = id;
    against.qty = order->leaves();
    against.ts_created = now;

    const auto plan = plan_cross(
        arriving,
        order->tif(),
        opposite_,
        desk,
        *ref,
        base_of(listed).tick_size,
        [&](std::uint16_t book, ClientOrderId standing, Side side, QtyLots qty, PriceTicks price) {
            OrderCmd leg{};
            leg.kind = CmdKind::Create;
            leg.side = side;
            leg.order_type = OrderType::Limit;
            leg.tif = TimeInForce::Ioc;
            leg.instrument = instrument;
            leg.client_order_id = standing.valid() ? standing : ClientOrderId::make(book, 0);
            leg.price = price;
            leg.qty = qty;
            leg.ts_created = now;
            return kernel_->leg_allowed(leg, standing);
        },
        std::span{planned_});
    out.why = plan.why;
    count(plan.why);

    for (std::size_t i = 0; i < plan.count; ++i) {
        const auto& allocation = planned_[i];
        if (!room_for(id.strategy_id(), allocation.book, instrument)) {
            out.why = Refusal::NoRoom;
            count(Refusal::NoRoom);
            break;
        }
        auto counter = allocation.counter;
        if (allocation.desk) {
            const auto opened = open_desk_leg(allocation.book, against, allocation.qty, allocation.price, now);
            if (!opened.has_value()) {
                out.why = Refusal::NoRoom;
                count(Refusal::NoRoom);
                break;
            }
            counter = *opened;
        }
        if (!book_cross(id, counter, allocation.desk, allocation, policy->fee_bp, *ref, listed, now)) {
            break;
        }
        if (!allocation.desk) {
            settle_held(counter, allocation.qty);
        }
        out.crossed = QtyLots::from_raw(out.crossed.raw + allocation.qty.raw);
    }

    const auto* after = store_->find(id);
    if (after != nullptr && !after->is_terminal() && after->leaves().raw > 0 && settings_.hold_ms != 0 &&
        order->tif() != TimeInForce::Fok && held_.size() < k_max_held) {
        auto interest = arriving;
        interest.leaves = after->leaves();
        interest.sell_cap = QtyLots::undef();
        held_.push_back(
            {.interest = interest, .instrument = instrument, .until_ns = now.ns + (settings_.hold_ms * 1'000'000ULL)});
        out.held = true;
    }
    return out;
}

} // namespace raikiri::internalcross
