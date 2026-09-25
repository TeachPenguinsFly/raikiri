#include <raikiri/simcore/engine.hpp>

#include <charconv>
#include <string_view>

namespace raikiri::simcore {

namespace {

[[nodiscard]] ExchangeOrderId format_sim_order_id(std::uint64_t id) noexcept {
    char buf[20]; // any uint64 fits in 20 decimal digits
    const auto res = std::to_chars(buf, buf + sizeof(buf), id);
    const auto text = std::string_view{buf, static_cast<std::size_t>(res.ptr - buf)};
    return *ExchangeOrderId::from(text); // cannot exceed 40 chars
}

[[nodiscard]] std::optional<std::uint64_t> parse_sim_order_id(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) {
        return std::nullopt; // not one of ours — venue answers "unknown order"
    }
    return value;
}

} // namespace

SimResult Engine::handle(const OrderCmd& cmd, UnixNanos now) {
    SimResult out;
    switch (cmd.kind) {
    case CmdKind::Create:
        handle_create(cmd, now, out);
        break;
    case CmdKind::Cancel:
        handle_cancel(cmd, now, out);
        break;
    case CmdKind::Amend:
        handle_amend(cmd, now, out);
        break;
    }
    return out;
}

void Engine::handle_create(const OrderCmd& cmd, UnixNanos now, SimResult& out) {
    Book& bk = book_for(cmd.instrument);
    const auto before = bk.bbo();

    ClientOrderMeta meta{};
    meta.instrument = cmd.instrument;
    meta.client_order_id = cmd.client_order_id;
    meta.exchange_client_order_id = cmd.exchange_client_order_id;
    meta.side = cmd.side;
    meta.order_type = cmd.order_type;
    meta.tif = cmd.tif;
    meta.flags = cmd.flags;
    meta.price = cmd.price;
    meta.qty = cmd.qty;
    meta.leaves = cmd.qty;

    const bool bad_qty = cmd.qty.is_undef() || cmd.qty.raw <= 0;
    const bool bad_price = cmd.order_type == OrderType::Limit && (cmd.price.is_undef() || cmd.price.raw <= 0);
    if (bad_qty || bad_price) {
        // Malformed orders die at the venue gate; the book never sees them.
        out.events.push_back(make_event(meta, OrderEventType::Rejected, now));
        return;
    }

    const std::uint64_t sim_id = next_sim_id_++;
    meta.exchange_order_id = format_sim_order_id(sim_id);
    const std::uint32_t owner = static_cast<std::uint32_t>(cmd.client_order_id.strategy_id()) + 1U;

    const MatchResult match = cmd.order_type == OrderType::Limit
                                  ? bk.submit_limit(sim_id,
                                                    owner,
                                                    cmd.side,
                                                    cmd.price,
                                                    cmd.qty,
                                                    has_flag(cmd.flags, OrderFlags::PostOnly),
                                                    cmd.tif)
                                  : bk.submit_market(sim_id, owner, cmd.side, cmd.qty);

    if (match.disposition == Disposition::RejectedPostOnly) {
        out.events.push_back(make_event(meta, OrderEventType::Rejected, now));
        return;
    }

    if (match.disposition == Disposition::Rested) {
        out.events.push_back(make_event(meta, OrderEventType::Accepted, now));
    }

    emit_fills(&meta, cmd.qty, cmd.side, cmd.instrument, match, now, out);

    if (match.disposition == Disposition::Expired) {
        meta.leaves = match.remainder;
        out.events.push_back(make_event(meta, OrderEventType::Expired, now));
    } else if (match.disposition == Disposition::Rested) {
        meta.leaves = match.remainder;
        client_orders_.emplace(sim_id, meta);
        by_exch_client_.emplace(meta.exchange_client_order_id, sim_id);
    }

    out.bbo_changed = out.bbo_changed || before != bk.bbo();
}

void Engine::handle_cancel(const OrderCmd& cmd, UnixNanos now, SimResult& out) {
    const auto sim_id = resolve_target(cmd);
    const auto it = sim_id ? client_orders_.find(*sim_id) : client_orders_.end();
    if (!sim_id || it == client_orders_.end()) {
        out.events.push_back(make_event_from_cmd(cmd, OrderEventType::CancelRejected, now));
        return;
    }
    const ClientOrderMeta meta = it->second;
    Book& bk = book_for(meta.instrument);
    const auto before = bk.bbo();
    if (!bk.cancel(*sim_id)) {
        out.events.push_back(make_event(meta, OrderEventType::CancelRejected, now));
        return;
    }
    by_exch_client_.erase(meta.exchange_client_order_id);
    client_orders_.erase(it);
    out.events.push_back(make_event(meta, OrderEventType::Canceled, now));
    out.bbo_changed = out.bbo_changed || before != bk.bbo();
}

void Engine::handle_amend(const OrderCmd& cmd, UnixNanos now, SimResult& out) {
    const auto sim_id = resolve_target(cmd);
    const auto it = sim_id ? client_orders_.find(*sim_id) : client_orders_.end();
    if (!sim_id || it == client_orders_.end()) {
        out.events.push_back(make_event_from_cmd(cmd, OrderEventType::ModifyRejected, now));
        return;
    }
    ClientOrderMeta& meta = it->second;

    // Undef fields mean "keep": an amend may move price, qty, or both.
    const PriceTicks new_price = cmd.price.is_undef() ? meta.price : cmd.price;
    const QtyLots new_qty = cmd.qty.is_undef() ? meta.qty : cmd.qty;
    const std::int64_t already_filled = meta.qty.raw - meta.leaves.raw;
    const std::int64_t new_leaves = new_qty.raw - already_filled;
    if (new_price.is_undef() || new_price.raw <= 0 || new_leaves <= 0) {
        // Venues refuse to amend total qty at or below the filled amount.
        out.events.push_back(make_event(meta, OrderEventType::ModifyRejected, now));
        return;
    }

    Book& bk = book_for(meta.instrument);
    const auto before = bk.bbo();
    const auto match = bk.amend(*sim_id, new_price, QtyLots::from_raw(new_leaves));
    if (!match.has_value()) {
        out.events.push_back(make_event(meta, OrderEventType::ModifyRejected, now));
        return;
    }

    meta.price = new_price;
    meta.qty = new_qty;
    meta.leaves = QtyLots::from_raw(new_leaves);
    out.events.push_back(make_event(meta, OrderEventType::Updated, now));
    emit_fills(&meta, meta.leaves, meta.side, meta.instrument, *match, now, out);

    meta.leaves = match->remainder;
    if (meta.leaves.raw == 0) {
        by_exch_client_.erase(meta.exchange_client_order_id);
        client_orders_.erase(it); // erasing other entries never invalidated `it`
    }
    out.bbo_changed = out.bbo_changed || before != bk.bbo();
}

std::uint64_t Engine::place_background_limit(
    InstrumentId instrument, Side side, PriceTicks price, QtyLots qty, UnixNanos now, SimResult& out) {
    Book& bk = book_for(instrument);
    const auto before = bk.bbo();
    const std::uint64_t sim_id = next_sim_id_++;
    const MatchResult match = bk.submit_limit(
        sim_id, k_background_owner, side, price, qty, /*post_only=*/false, TimeInForce::Gtc);
    emit_fills(nullptr, qty, side, instrument, match, now, out);
    out.bbo_changed = out.bbo_changed || before != bk.bbo();
    return sim_id;
}

void Engine::place_background_market(InstrumentId instrument, Side side, QtyLots qty, UnixNanos now, SimResult& out) {
    Book& bk = book_for(instrument);
    const auto before = bk.bbo();
    const std::uint64_t sim_id = next_sim_id_++;
    const MatchResult match = bk.submit_market(sim_id, k_background_owner, side, qty);
    emit_fills(nullptr, qty, side, instrument, match, now, out);
    out.bbo_changed = out.bbo_changed || before != bk.bbo();
}

bool Engine::cancel_background(InstrumentId instrument, std::uint64_t sim_id, SimResult* out) {
    const auto it = books_.find(instrument);
    if (it == books_.end()) {
        return false;
    }
    Book& bk = it->second;
    const Book::RestingOrder* order = bk.find(sim_id);
    if (order == nullptr || order->owner != k_background_owner) {
        return false; // background flow must never cancel client orders
    }
    const auto before = bk.bbo();
    const bool removed = bk.cancel(sim_id);
    if (removed && out != nullptr) {
        out->bbo_changed = out->bbo_changed || before != bk.bbo();
    }
    return removed;
}

const Book* Engine::book(InstrumentId instrument) const noexcept {
    const auto it = books_.find(instrument);
    return it == books_.end() ? nullptr : &it->second;
}

std::optional<Book::BboLevels> Engine::bbo(InstrumentId instrument) const {
    const Book* bk = book(instrument);
    return bk == nullptr ? std::nullopt : bk->bbo();
}

Book& Engine::book_for(InstrumentId instrument) {
    return books_.try_emplace(instrument).first->second;
}

ExecId Engine::next_exec_id() noexcept {
    char buf[24];
    buf[0] = 'S';
    buf[1] = '-';
    const auto res = std::to_chars(buf + 2, buf + sizeof(buf), next_exec_seq_++);
    const auto text = std::string_view{buf, static_cast<std::size_t>(res.ptr - buf)};
    return *ExecId::from(text); // "S-" + <=20 digits always fits 40 chars
}

OrderEvent Engine::make_event(const ClientOrderMeta& meta, OrderEventType type, UnixNanos now) const {
    OrderEvent ev{};
    ev.type = type;
    ev.side = meta.side;
    ev.order_type = meta.order_type;
    ev.tif = meta.tif;
    ev.flags = meta.flags;
    ev.instrument = meta.instrument;
    ev.client_order_id = meta.client_order_id;
    ev.exchange_client_order_id = meta.exchange_client_order_id;
    ev.exchange_order_id = meta.exchange_order_id;
    ev.price = meta.price;
    ev.qty = meta.qty;
    ev.leaves_qty = meta.leaves;
    ev.fee = FixedE9::from_raw(0); // sim charges no fees — explicit zero, not undef
    ev.ts_event = now;             // the sim venue's clock IS the caller's clock
    ev.ts_init = now;
    return ev;
}

OrderEvent Engine::make_event_from_cmd(const OrderCmd& cmd, OrderEventType type, UnixNanos now) const {
    ClientOrderMeta meta{};
    meta.instrument = cmd.instrument;
    meta.client_order_id = cmd.client_order_id;
    meta.exchange_client_order_id = cmd.exchange_client_order_id;
    meta.exchange_order_id = cmd.target_exchange_order_id;
    meta.side = cmd.side;
    meta.order_type = cmd.order_type;
    meta.tif = cmd.tif;
    meta.flags = cmd.flags;
    meta.price = cmd.price;
    meta.qty = cmd.qty;
    meta.leaves = cmd.qty;
    return make_event(meta, type, now);
}

std::optional<std::uint64_t> Engine::resolve_target(const OrderCmd& cmd) const {
    if (!cmd.target_exchange_order_id.empty()) {
        return parse_sim_order_id(cmd.target_exchange_order_id.view());
    }
    // Fall back to the OMS-minted id — venues accept either handle.
    const auto it = by_exch_client_.find(cmd.exchange_client_order_id);
    if (it == by_exch_client_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void Engine::fold_position(InstrumentId instrument, Side side, QtyLots qty) {
    auto& held = positions_[instrument];
    const auto base = held.is_undef() ? 0 : held.raw; // default-constructed is undef, not flat
    held = QtyLots::from_raw(base + (side == Side::Buy ? qty.raw : -qty.raw));
}

void Engine::emit_fills(const ClientOrderMeta* taker,
                        QtyLots taker_leaves_start,
                        Side taker_side,
                        InstrumentId instrument,
                        const MatchResult& match,
                        UnixNanos now,
                        SimResult& out) {
    std::int64_t taker_left = taker_leaves_start.raw;
    for (const Fill& fill : match.fills) {
        taker_left -= fill.qty.raw;

        if (taker != nullptr) {
            fold_position(instrument, taker_side, fill.qty);
            OrderEvent ev = make_event(*taker, OrderEventType::Filled, now);
            ev.liquidity = Liquidity::Taker;
            ev.price = fill.price; // executions happen at RESTING prices
            ev.qty = fill.qty;
            ev.leaves_qty = QtyLots::from_raw(taker_left);
            ev.exec_id = next_exec_id();
            out.events.push_back(ev);
        }

        if (fill.maker_owner != k_background_owner) {
            const auto maker_it = client_orders_.find(fill.maker_id);
            if (maker_it != client_orders_.end()) {
                ClientOrderMeta& maker = maker_it->second;
                fold_position(instrument, maker.side, fill.qty);
                maker.leaves = fill.maker_leaves;
                OrderEvent ev = make_event(maker, OrderEventType::Filled, now);
                ev.liquidity = Liquidity::Maker;
                ev.price = fill.price;
                ev.qty = fill.qty;
                ev.leaves_qty = fill.maker_leaves;
                ev.exec_id = next_exec_id(); // each fill side gets its own ExecId
                out.events.push_back(ev);
                if (fill.maker_leaves.raw == 0) {
                    by_exch_client_.erase(maker.exchange_client_order_id);
                    client_orders_.erase(maker_it);
                }
            }
        }

        TradeTick trade{};
        trade.instrument = instrument;
        trade.price = fill.price;
        trade.qty = fill.qty;
        trade.aggressor = taker_side;
        trade.ts_event = now;
        trade.ts_init = now;
        out.trades.push_back(trade);
    }
}

} // namespace raikiri::simcore
