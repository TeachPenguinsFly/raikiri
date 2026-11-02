// strategyd: strategy kernel; attaches gateways, runs strategies and serves a loopback control port
// single thread: rings, timers and the control surface are polled in one loop

#include <raikiri/audit/audited.hpp>
#include <raikiri/audit/file_sink.hpp>
#include <raikiri/bookrisk/desk.hpp>
#include <raikiri/bookrisk/file_store.hpp>
#include <raikiri/bookrisk/notifiers.hpp>
#include <raikiri/bookrisk/routes.hpp>
#include <raikiri/bookrisk/shm_balances.hpp>
#include <raikiri/bookrisk/value.hpp>
#include <raikiri/books/file_store.hpp>
#include <raikiri/common/version.hpp>
#include <raikiri/config/risk.hpp>
#include <raikiri/config/settings.hpp>
#include <raikiri/domain/refusal_codes.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/internalcross/application/control.hpp>
#include <raikiri/internalcross/application/desk.hpp>
#include <raikiri/internalcross/infrastructure/file_store.hpp>
#include <raikiri/internalcross/infrastructure/journal_log.hpp>
#include <raikiri/internalcross/infrastructure/shm_touches.hpp>
#include <raikiri/internalcross/presentation/routes.hpp>
#include <raikiri/joe/desk.hpp>
#include <raikiri/joe/parent_journal.hpp>
#include <raikiri/joe/routes.hpp>
#include <raikiri/joe/runner.hpp>
#include <raikiri/joe/shm_market.hpp>
#include <raikiri/journal/internal.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/observ/histogram.hpp>
#include <raikiri/oms/attribution.hpp>
#include <raikiri/oms/exec_path.hpp>
#include <raikiri/oms/fee_ledger.hpp>
#include <raikiri/oms/funding_identity.hpp>
#include <raikiri/oms/funding_split.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/kernel_gate.hpp>
#include <raikiri/oms/order_commands.hpp>
#include <raikiri/oms/order_store.hpp>
#include <raikiri/oms/order_trail.hpp>
#include <raikiri/oms/recon.hpp>
#include <raikiri/oms/repeat_guard.hpp>
#include <raikiri/oms/replay.hpp>
#include <raikiri/oms/risk.hpp>
#include <raikiri/oms/short_option.hpp>
#include <raikiri/optionrisk/short_book.hpp>
#include <raikiri/optionrisk/short_book_routes.hpp>
#include <raikiri/portfolio/journal.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/portfolio/reconcile.hpp>
#include <raikiri/portfolio/target.hpp>
#include <raikiri/schema/codec.hpp>
#include <raikiri/shm/lease.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/strategies/register.hpp>
#include <raikiri/strategy/control.hpp>
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/registry.hpp>
#include <raikiri/strategy/strategy.hpp>

#include <fmt/format.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "control_audit.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

using msgs = raikiri_journal::schema::messages;
using namespace raikiri;

class Observer final : public strategy::Strategy {};

// per-(venue, market) state; the book, order store and risk gate span gateways
struct Gateway {
    AccountId account{};
    std::string name;
    shm::SegmentClient client;
    // sole producer of cmd_create and cmd_cancel, sole consumer of order_intents
    shm::WriterLease lease;
    shm::GatewayChannels ch{};
    shm::ExecReader exec;
    shm::LedgerEntryReader charges;
    shm::FillReader fills;
    shm::MdReader md;
    shm::CmdProducer creates;
    shm::CmdProducer cancels;
    shm::CmdConsumer intents;
    std::vector<std::uint32_t> bbo_seen;
    std::vector<std::uint32_t> funding_seen;
    std::uint64_t recon_due{0};
    // came back holding what the journal said, and the venue has not been heard since
    bool restored{false};
    // one kernel gate per gateway so a break on one does not stop another
    oms::KernelGate kernel;

    Gateway(AccountId id,
            std::string display,
            shm::SegmentClient attached,
            shm::WriterLease held,
            shm::GatewayChannels channels)
        : account{id}, name{std::move(display)}, client{std::move(attached)}, lease{std::move(held)}, ch{channels},
          exec{channels.exec_events}, charges{channels.funding_charges}, fills{channels.fills}, md{channels.md_events},
          creates{channels.cmd_create}, cancels{channels.cmd_cancel}, intents{channels.order_intents},
          bbo_seen(std::min<std::size_t>(channels.catalogue->size(), shm::k_bbo_cells)),
          funding_seen(std::min<std::size_t>(channels.catalogue->size(), shm::k_funding_cells)) {}
};

class KernelContext {
public:
    KernelContext(const KernelContext&) = delete;
    KernelContext& operator=(const KernelContext&) = delete;

    KernelContext(std::vector<Gateway>& gateways,
                  oms::OrderStore& store,
                  config::RiskFile risk,
                  oms::IdGen& ids,
                  journal::JournalWriter& journal,
                  audit::Sink& audit,
                  const Clock& clock)
        : exec{store}, trail{audit}, gateways_{&gateways}, store_{&store}, risk_file_{std::move(risk)}, ids_{&ids},
          journal_{&journal}, audit_{&audit}, clock_{&clock} {}

    oms::ExecPath exec;
    oms::OrderTrail trail;
    oms::RepeatGuard repeats;
    std::uint64_t duplicates_refused{0};
    // the highest order seq each strategy reached before this process
    std::map<std::uint16_t, std::uint64_t> last_seq;

    void audit_event(std::string_view actor,
                     std::string_view action,
                     std::string_view subject,
                     std::string_view detail) noexcept {
        audit_->append(
            {.ts = clock_->wall(), .actor = actor, .action = action, .subject = subject, .detail_json = detail});
    }

    [[nodiscard]] Gateway* gateway_for(InstrumentId id) const noexcept {
        const AccountId want{.venue = id.venue(), .market = id.market()};
        for (auto& gw : *gateways_) {
            if (gw.account == want) {
                return &gw;
            }
        }
        return nullptr;
    }

    /** place an order under a client order id the desk already minted */
    std::expected<ClientOrderId, strategy::RejectReason>
    submit_as(const strategy::NewOrder& order, ClientOrderId id, const OrderOrigin& origin = {}) noexcept {
        const auto t0 = clock_->mono();

        OrderCmd cmd{};
        cmd.kind = CmdKind::Create;
        cmd.side = order.side;
        cmd.order_type = order.type;
        cmd.tif = order.tif;
        cmd.flags = order.flags;
        cmd.instrument = order.instrument;
        cmd.client_order_id = id;
        cmd.exchange_client_order_id = ids_->next_exchange_client_id(clock_->wall());
        cmd.price = order.price;
        cmd.qty = order.qty;
        cmd.priority_e8 = order.priority_e8;
        cmd.ts_created = clock_->wall();

        auto* gw = gateway_for(order.instrument);
        if (gw == nullptr) {
            audit_refused(cmd, {}, oms::RiskRefusal{}, "no_gateway");
            return std::unexpected{strategy::RejectReason::VenueDown};
        }
        const auto& gate = book_risk->gate(id.strategy_id());
        const oms::OrderShape shape{.book = id.strategy_id(),
                                    .instrument = order.instrument,
                                    .side = order.side,
                                    .price = order.price,
                                    .qty = order.qty,
                                    .repeat_key = order.repeat_key};
        if (const auto earlier = repeats.held(shape, cmd.ts_created, gate.config().duplicate_window_ns);
            earlier.has_value()) {
            ++duplicates_refused;
            last_refusal_code_ = k_refused_duplicate;
            audit_refused(
                cmd, {}, oms::RiskRefusal{.value = static_cast<std::int64_t>(earlier->raw)}, "duplicate_order");
            return std::unexpected{strategy::RejectReason::Duplicate};
        }
        const auto held = held_by(id.strategy_id());
        oms::Exposure exposure{.reconciliation_break = recon_gate.halted(order.instrument) || !gw->kernel.may_open(),
                               .open_orders = open_count_of(id.strategy_id()),
                               .instrument = instrument(order.instrument)};
        if (const auto it = held.find(order.instrument.packed); it != held.end()) {
            exposure.position_lots = it->second.position;
            exposure.working_buy_lots = it->second.buy;
            exposure.working_sell_lots = it->second.sell;
        }
        if (const auto quote = bbo(order.instrument);
            quote.has_value() && !quote->bid.is_undef() && !quote->ask.is_undef()) {
            exposure.reference_price = PriceTicks::from_raw((quote->bid.raw + quote->ask.raw) / 2);
            exposure.bid = quote->bid;
            exposure.ask = quote->ask;
            exposure.quote_ts = quote->ts_init;
        }
        exposure.underlying_price = underlying_price(order.instrument);
        fill_short_option(exposure, order.instrument, held, id.strategy_id());
        if (!gate.config().max_strategy_notional.is_undef()) {
            exposure.strategy_open_notional = open_notional_besides(held, order.instrument, cmd.ts_created, gate);
        }
        book_risk->admit(id.strategy_id(), cmd, exposure);
        if (const auto allowed = gate.check(cmd, exposure); !allowed.has_value()) {
            RLOG_WARN("strategyd: risk refused coid={} on {} — {}",
                      cmd.client_order_id.raw,
                      exposure.instrument != nullptr ? base_of(*exposure.instrument).exchange_symbol.view() : "?",
                      oms::describe(allowed.error()));
            audit_refused(cmd,
                          exposure.instrument != nullptr ? base_of(*exposure.instrument).exchange_symbol.view()
                                                         : std::string_view{},
                          allowed.error(),
                          oms::to_string(allowed.error().why));
            book_risk->refused(id.strategy_id(), allowed.error());
            last_refusal_code_ = refusal_code_of(allowed.error().why);
            return std::unexpected{strategy::RejectReason::Risk};
        }
        const bool crossing = cross != nullptr && exposure.instrument != nullptr &&
                              cross->wants(id.strategy_id(), *exposure.instrument);
        // the account can be flat while this book holds a position it crossed inside the desk, so the book is the check
        if (crossing && has_flag(cmd.flags, OrderFlags::ReduceOnly) && !oms::shrinks_position(cmd, exposure)) {
            audit_refused(cmd, {}, oms::RiskRefusal{}, "reduce_only_not_reducing");
            last_refusal_code_ = k_refused_risk;
            return std::unexpected{strategy::RejectReason::Risk};
        }
        if (const auto created = store_->create(cmd, cmd.exchange_client_order_id); !created.has_value()) {
            const bool same_id = created.error() == oms::StoreError::Duplicate;
            audit_refused(cmd, {}, oms::RiskRefusal{}, same_id ? "duplicate_order_id" : "order_store_full");
            last_refusal_code_ = same_id ? k_refused_duplicate : k_refused_capacity;
            return std::unexpected{same_id ? strategy::RejectReason::Duplicate : strategy::RejectReason::Capacity};
        }
        trail.command("order.created", cmd, cmd.ts_created);
        // journal the intent before the wire
        journal_cmd(cmd, origin);
        if (crossing) {
            return submit_crossing(cmd, exposure, shape, t0);
        }
        if (!gw->creates.try_push(cmd).has_value()) {
            // it never left: held open it would count as working until a recon, and a restart would bring it back
            const auto never_sent = denied_of(cmd);
            (void)store_->apply_event(never_sent);
            journal_event(never_sent);
            audit_refused(cmd, {}, oms::RiskRefusal{}, "ring_full");
            last_refusal_code_ = k_refused_venue_down;
            return std::unexpected{strategy::RejectReason::VenueDown};
        }
        book_risk->sent(id.strategy_id(), cmd, exposure);
        // apply the local submitted transition; the fsm has no accepted arm from initialized
        OrderEvent submitted{};
        submitted.type = OrderEventType::Submitted;
        submitted.side = cmd.side;
        submitted.order_type = cmd.order_type;
        submitted.tif = cmd.tif;
        submitted.flags = cmd.flags;
        submitted.instrument = cmd.instrument;
        submitted.client_order_id = cmd.client_order_id;
        submitted.exchange_client_order_id = cmd.exchange_client_order_id;
        submitted.qty = cmd.qty;
        submitted.leaves_qty = cmd.qty;
        submitted.price = cmd.price;
        submitted.ts_init = cmd.ts_created;
        (void)store_->apply_event(submitted);
        trail.command("order.sent", cmd, cmd.ts_created);
        repeats.sent(shape, id, cmd.ts_created);

        inflight_[cmd.client_order_id.raw] = t0.ns;
        submit_hist.record(clock_->mono().ns - t0.ns);
        return cmd.client_order_id;
    }

    // what a cross left of an order goes to the venue, sized to what the venue has not seen
    std::expected<ClientOrderId, strategy::RejectReason> send_rest(ClientOrderId id, std::uint32_t priority) noexcept {
        const auto* order = store_->find(id);
        if (order == nullptr || order->is_terminal()) {
            return id;
        }
        const auto now = clock_->wall();
        OrderCmd wire{};
        wire.kind = CmdKind::Create;
        wire.side = order->side();
        wire.order_type = order->order_type();
        wire.tif = order->tif();
        // the book was the reduce-only check; the venue's position is the sum of books and would refuse
        wire.flags = static_cast<OrderFlags>(static_cast<std::uint8_t>(order->flags()) &
                                             ~static_cast<std::uint8_t>(OrderFlags::ReduceOnly));
        wire.instrument = order->instrument();
        wire.client_order_id = id;
        wire.exchange_client_order_id = order->exchange_client_order_id();
        wire.price = order->price();
        wire.qty = order->leaves();
        wire.priority_e8 = priority;
        wire.ts_created = now;
        auto* gw = gateway_for(wire.instrument);
        if (gw != nullptr && gw->creates.try_push(wire).has_value()) {
            trail.command("order.sent", wire, now);
            return id;
        }
        const bool part_filled = order->filled().raw != 0;
        OrderEvent gone = denied_of(wire);
        // a fill stands, so what is closed is the rest
        gone.type = part_filled ? OrderEventType::Canceled : OrderEventType::Denied;
        const auto done = exec.apply(gone, false, now);
        journal_event(gone);
        trail.event(gone, done.order, now);
        audit_refused(wire, {}, oms::RiskRefusal{}, gw == nullptr ? "no_gateway" : "ring_full");
        repeats.withdrawn(id);
        last_refusal_code_ = k_refused_venue_down;
        if (part_filled) {
            cross_events.push_back(gone);
            return id;
        }
        return std::unexpected{strategy::RejectReason::VenueDown};
    }

    std::expected<ClientOrderId, strategy::RejectReason> submit_crossing(const OrderCmd& cmd,
                                                                         const oms::Exposure& exposure,
                                                                         const oms::OrderShape& shape,
                                                                         MonoNanos t0) noexcept {
        OrderEvent submitted{};
        submitted.type = OrderEventType::Submitted;
        submitted.side = cmd.side;
        submitted.order_type = cmd.order_type;
        submitted.tif = cmd.tif;
        submitted.flags = cmd.flags;
        submitted.instrument = cmd.instrument;
        submitted.client_order_id = cmd.client_order_id;
        submitted.exchange_client_order_id = cmd.exchange_client_order_id;
        submitted.qty = cmd.qty;
        submitted.leaves_qty = cmd.qty;
        submitted.price = cmd.price;
        submitted.ts_init = cmd.ts_created;
        (void)store_->apply_event(submitted);
        book_risk->sent(cmd.client_order_id.strategy_id(), cmd, exposure);
        repeats.sent(shape, cmd.client_order_id, cmd.ts_created);
        const auto arrived = cross->on_arrival(cmd.client_order_id, *exposure.instrument, cmd.ts_created);
        if (arrived.held) {
            return cmd.client_order_id;
        }
        const auto sent = send_rest(cmd.client_order_id, cmd.priority_e8);
        // crossed whole, no venue will ever answer it
        if (sent.has_value() && store_->find(cmd.client_order_id) != nullptr) {
            inflight_[cmd.client_order_id.raw] = t0.ns;
            submit_hist.record(clock_->mono().ns - t0.ns);
        }
        return sent;
    }

    // a held order's wait ended with nobody opposite
    void release_held() noexcept {
        for (const auto id : cross->take_due(clock_->wall())) {
            (void)send_rest(id, 0);
        }
    }

    // the counterparty's own gate for the lots it would take; it has passed for its order as a whole, but that was
    // earlier, and the hedge book has no order at all
    bool leg_allowed(const OrderCmd& leg, ClientOrderId standing) {
        const auto book = leg.client_order_id.strategy_id();
        const auto* gw = gateway_for(leg.instrument);
        if (gw == nullptr) {
            return false;
        }
        const auto held = held_by(book);
        oms::Exposure exposure{.reconciliation_break = recon_gate.halted(leg.instrument) || !gw->kernel.may_open(),
                               .open_orders = open_count_of(book),
                               .instrument = instrument(leg.instrument)};
        if (const auto it = held.find(leg.instrument.packed); it != held.end()) {
            exposure.position_lots = it->second.position;
            exposure.working_buy_lots = it->second.buy;
            exposure.working_sell_lots = it->second.sell;
        }
        if (standing.valid()) {
            if (const auto* own = store_->find(standing); own != nullptr) {
                auto& side = own->side() == Side::Buy ? exposure.working_buy_lots : exposure.working_sell_lots;
                side = QtyLots::from_raw(side.raw - own->leaves().raw);
                exposure.open_orders -= 1;
            }
        }
        if (const auto quote = bbo(leg.instrument);
            quote.has_value() && !quote->bid.is_undef() && !quote->ask.is_undef()) {
            exposure.reference_price = PriceTicks::from_raw((quote->bid.raw + quote->ask.raw) / 2);
            exposure.bid = quote->bid;
            exposure.ask = quote->ask;
            exposure.quote_ts = quote->ts_init;
        }
        exposure.underlying_price = underlying_price(leg.instrument);
        fill_short_option(exposure, leg.instrument, held, book);
        const auto& gate = book_risk->gate(book);
        if (!gate.config().max_strategy_notional.is_undef()) {
            exposure.strategy_open_notional = open_notional_besides(held, leg.instrument, leg.ts_created, gate);
        }
        book_risk->admit(book, leg, exposure);
        const auto allowed = gate.check(leg, exposure);
        if (!allowed.has_value()) {
            audit_refused(leg, {}, allowed.error(), "internal_leg_refused");
        }
        return allowed.has_value();
    }

    bool cancel(ClientOrderId id) noexcept {
        const auto* order = store_->find(id);
        if (order == nullptr) {
            // the close beat this cancel, which is what it wanted
            return exec.closed().holds(id);
        }
        const auto now = clock_->wall();
        if (cross != nullptr && cross->holds(id)) {
            // no venue has it, so there is nothing to wait for
            cross->forget(id);
            OrderEvent gone = denied_of(OrderCmd{.side = order->side(),
                                                 .order_type = order->order_type(),
                                                 .tif = order->tif(),
                                                 .flags = order->flags(),
                                                 .instrument = order->instrument(),
                                                 .client_order_id = id,
                                                 .exchange_client_order_id = order->exchange_client_order_id(),
                                                 .price = order->price(),
                                                 .qty = order->qty()});
            gone.type = OrderEventType::Canceled;
            gone.leaves_qty = order->leaves();
            const auto done = exec.apply(gone, false, now);
            journal_event(gone);
            trail.event(gone, done.order, now);
            repeats.withdrawn(id);
            cross_events.push_back(gone);
            return true;
        }
        if (oms::cancel_effect(*order, now) == oms::CommandEffect::NoOp) {
            return true;
        }
        OrderCmd cmd{};
        cmd.kind = CmdKind::Cancel;
        cmd.instrument = order->instrument();
        cmd.client_order_id = id;
        cmd.exchange_client_order_id = order->exchange_client_order_id();
        cmd.target_exchange_order_id = order->exchange_order_id();
        cmd.ts_created = now;
        auto* gw = gateway_for(cmd.instrument);
        if (gw == nullptr) {
            return false;
        }
        journal_cmd(cmd);
        if (!gw->cancels.try_push(cmd).has_value()) {
            return false;
        }
        OrderEvent pending{};
        pending.type = OrderEventType::PendingCancel;
        pending.side = order->side();
        pending.instrument = cmd.instrument;
        pending.client_order_id = id;
        pending.exchange_client_order_id = cmd.exchange_client_order_id;
        pending.exchange_order_id = cmd.target_exchange_order_id;
        pending.qty = order->qty();
        pending.leaves_qty = order->leaves();
        pending.price = order->price();
        pending.ts_init = now;
        const auto applied = store_->apply_event(pending);
        trail.event(pending, applied.has_value() ? *applied : nullptr, now);
        // asked to stop, so an order of the same shape from here on is a new decision
        repeats.withdrawn(id);
        return true;
    }

    bool amend(ClientOrderId id, PriceTicks price) noexcept {
        const auto* order = store_->find(id);
        if (order == nullptr || order->is_terminal() || (cross != nullptr && cross->holds(id))) {
            return false;
        }
        if (oms::amend_effect(*order, price) == oms::CommandEffect::NoOp) {
            return true;
        }
        auto* gw = gateway_for(order->instrument());
        if (gw == nullptr) {
            return false;
        }
        OrderCmd cmd{};
        cmd.kind = CmdKind::Amend;
        cmd.side = order->side();
        cmd.order_type = order->order_type();
        cmd.tif = order->tif();
        cmd.flags = order->flags();
        cmd.instrument = order->instrument();
        cmd.client_order_id = id;
        cmd.exchange_client_order_id = order->exchange_client_order_id();
        cmd.target_exchange_order_id = order->exchange_order_id();
        cmd.price = price;
        cmd.qty = order->venue_qty();
        cmd.ts_created = clock_->wall();
        // the order's own size is already working, so it is not counted twice
        const auto held = held_by(id.strategy_id());
        oms::Exposure exposure{.reconciliation_break = recon_gate.halted(cmd.instrument) || !gw->kernel.may_open(),
                               .open_orders = open_count_of(id.strategy_id()) - 1,
                               .instrument = instrument(cmd.instrument)};
        if (const auto it = held.find(cmd.instrument.packed); it != held.end()) {
            exposure.position_lots = it->second.position;
            exposure.working_buy_lots = it->second.buy;
            exposure.working_sell_lots = it->second.sell;
        }
        auto& own = cmd.side == Side::Buy ? exposure.working_buy_lots : exposure.working_sell_lots;
        own = QtyLots::from_raw(own.raw - order->leaves().raw);
        if (const auto quote = bbo(cmd.instrument);
            quote.has_value() && !quote->bid.is_undef() && !quote->ask.is_undef()) {
            exposure.reference_price = PriceTicks::from_raw((quote->bid.raw + quote->ask.raw) / 2);
            exposure.bid = quote->bid;
            exposure.ask = quote->ask;
            exposure.quote_ts = quote->ts_init;
        }
        exposure.underlying_price = underlying_price(cmd.instrument);
        fill_short_option(exposure, cmd.instrument, held, id.strategy_id());
        const auto& gate = book_risk->gate(id.strategy_id());
        if (!gate.config().max_strategy_notional.is_undef()) {
            exposure.strategy_open_notional = open_notional_besides(held, cmd.instrument, cmd.ts_created, gate);
        }
        book_risk->admit(id.strategy_id(), cmd, exposure);
        if (const auto allowed = gate.check(cmd, exposure); !allowed.has_value()) {
            RLOG_WARN("strategyd: risk refused amend coid={} — {}", id.raw, oms::describe(allowed.error()));
            book_risk->refused(id.strategy_id(), allowed.error());
            return false;
        }
        journal_cmd(cmd);
        if (!gw->creates.try_push(cmd).has_value()) {
            return false;
        }
        book_risk->sent(id.strategy_id(), cmd, exposure);
        OrderEvent pending{};
        pending.type = OrderEventType::PendingUpdate;
        pending.side = cmd.side;
        pending.order_type = cmd.order_type;
        pending.tif = cmd.tif;
        pending.flags = cmd.flags;
        pending.instrument = cmd.instrument;
        pending.client_order_id = id;
        pending.exchange_client_order_id = cmd.exchange_client_order_id;
        pending.qty = cmd.qty;
        pending.leaves_qty = order->leaves();
        pending.price = cmd.price;
        pending.ts_init = cmd.ts_created;
        const auto applied = store_->apply_event(pending);
        trail.event(pending, applied.has_value() ? *applied : nullptr, cmd.ts_created);
        return true;
    }

    std::optional<Bbo> bbo(InstrumentId instrument) const noexcept {
        const auto idx = instrument.symbol().raw;
        const auto* gw = gateway_for(instrument);
        if (gw == nullptr || idx >= shm::k_bbo_cells) {
            return std::nullopt;
        }
        Bbo out{};
        if (gw->ch.bbo->read(idx, out) == 0) {
            return std::nullopt;
        }
        return out;
    }

    const Instrument* instrument(InstrumentId id) const noexcept {
        const auto* gw = gateway_for(id);
        return gw != nullptr ? gw->ch.catalogue->find(id) : nullptr;
    }

    // the venue's own underlying price for an option, which values a coin premium in quote
    FixedE9 underlying_price(InstrumentId id) const noexcept {
        const auto idx = id.symbol().raw;
        const auto* gw = gateway_for(id);
        if (id.market() != MarketType::Option || gw == nullptr || gw->ch.greeks == nullptr ||
            idx >= shm::k_greeks_cells) {
            return FixedE9::undef();
        }
        OptionGreeks greeks{};
        if (gw->ch.greeks->read(idx, greeks) == 0 || greeks.instrument != id) {
            return FixedE9::undef();
        }
        return greeks.underlying_price;
    }

    // reprices every option a book holds, so it runs on a timer and not on the order path
    void check_short_greeks(UnixNanos now) {
        std::unordered_map<std::uint16_t, std::vector<optionrisk::PositionRisk>> by_book;
        for (const auto& position : positions.all()) {
            const auto id = position.key().instrument;
            const auto* option = id.market() == MarketType::Option ? option_of(id) : nullptr;
            if (option == nullptr || position.qty().raw == 0) {
                continue;
            }
            const auto holding = optionrisk::holding_of(*option, position.qty(), now);
            const auto greeks = greeks_of(id);
            auto& book = by_book[position.key().strategy_id];
            book.push_back(holding.has_value() ? optionrisk::assess(*holding,
                                                                    greeks.has_value() ? optionrisk::market_of(*greeks)
                                                                                       : optionrisk::MarketState{},
                                                                    FixedE9::undef(),
                                                                    now,
                                                                    optionrisk::Staleness{})
                                               : optionrisk::PositionRisk{});
        }
        for (auto& [strategy_id, state] : short_greeks_) {
            state.greeks = {};
            if (!by_book.contains(strategy_id) && state.breached) {
                state.breached = false;
                RLOG_INFO("strategyd: book {} holds no options now, so it may sell again", strategy_id);
            }
        }
        for (const auto& [strategy_id, book] : by_book) {
            auto& state = short_greeks_[strategy_id];
            state.greeks = optionrisk::book_greeks(book);
            const auto& limits = book_risk->gate(strategy_id).config();
            const bool breached = optionrisk::past_short_limits(
                state.greeks, limits.max_short_vega, limits.max_short_gamma);
            if (breached != state.breached) {
                state.breached = breached;
                RLOG_WARN("strategyd: book {} {} its short vega or gamma limit, net vega {} gamma {}, {} unvalued",
                          strategy_id,
                          breached ? "is past" : "is back inside",
                          state.greeks.vega_usd.raw / k_e9,
                          state.greeks.gamma_usd_1pct.raw / k_e9,
                          state.greeks.incomplete);
                if (breached) {
                    book_risk->reduce_book(strategy_id, "past its short vega or gamma limit");
                }
            }
        }
    }

    // a row per book, venue and settlement asset it has sold in
    [[nodiscard]] std::vector<optionrisk::ShortBookRow> short_book_rows(UnixNanos now) {
        std::set<std::uint16_t> sold_books;
        for (const auto& position : positions.all()) {
            if (position.key().instrument.market() == MarketType::Option && position.qty().raw < 0) {
                sold_books.insert(position.key().strategy_id);
            }
        }
        store_->for_each_open([&](const oms::Order& open) {
            if (open.instrument().market() == MarketType::Option && open.side() == Side::Sell) {
                sold_books.insert(open.client_order_id().strategy_id());
            }
        });
        std::vector<optionrisk::ShortBookRow> out;
        for (const auto strategy_id : sold_books) {
            const auto first = out.size();
            FixedE9 book_naked = FixedE9::from_raw(0);
            for (const auto& [packed, lots] : held_by(strategy_id)) {
                const InstrumentId id{packed};
                const auto* option = id.market() == MarketType::Option ? option_of(id) : nullptr;
                if (option == nullptr) {
                    continue;
                }
                const auto figures = oms::short_figures(short_line(*option, lots), short_rates(*option));
                if (figures.held.raw == 0 && figures.working.raw == 0) {
                    continue;
                }
                const auto asset = oms::settlement_asset(*option);
                const auto venue = to_string(id.venue());
                auto row = std::find_if(out.begin() + static_cast<std::ptrdiff_t>(first),
                                        out.end(),
                                        [&](const auto& r) { return r.venue == venue && r.asset == asset; });
                if (row == out.end()) {
                    const auto greeks = greeks_of(id);
                    out.push_back({.book_id = strategy_id,
                                   .book = strategy_name ? strategy_name(strategy_id) : std::to_string(strategy_id),
                                   .venue = std::string{venue},
                                   .asset = std::string{asset},
                                   .naked_usd = FixedE9::from_raw(0),
                                   .margin_held = FixedE9::from_raw(0),
                                   .margin_working = FixedE9::from_raw(0),
                                   .margin_available = available_margin(id, asset, now),
                                   .asset_usd = option->premium == OptionUnit::Quote ? FixedE9::from_raw(k_e9)
                                                : greeks.has_value()                 ? greeks->index_price
                                                                                     : FixedE9::undef()});
                    row = std::prev(out.end());
                }
                row->short_lots += figures.held.raw;
                row->working_lots += figures.working.raw;
                row->naked_usd = plus(row->naked_usd, figures.naked_usd);
                row->margin_held = plus(row->margin_held, figures.margin_held);
                row->margin_working = plus(row->margin_working, figures.margin_working);
                book_naked = plus(book_naked, figures.naked_usd);
            }
            const auto& limits = book_risk->gate(strategy_id).config();
            const auto state = short_greeks_.find(strategy_id);
            for (auto row = out.begin() + static_cast<std::ptrdiff_t>(first); row != out.end(); ++row) {
                row->book_naked_usd = book_naked;
                row->max_naked_short = limits.max_naked_short;
                row->max_short_vega = limits.max_short_vega;
                row->max_short_gamma = limits.max_short_gamma;
                if (state != short_greeks_.end()) {
                    row->vega_usd = state->second.greeks.vega_usd;
                    row->gamma_usd_1pct = state->second.greeks.gamma_usd_1pct;
                    row->reducing = state->second.breached;
                }
            }
        }
        return out;
    }

    [[nodiscard]] const Clock& clock() const noexcept { return *clock_; }

    [[nodiscard]] std::vector<internalcross::LiabilityView> cross_liabilities(const books::Registry& registry) const {
        std::vector<internalcross::LiabilityView> out;
        const auto rows = internalcross::liability_rows(
            positions.all(), [&registry](std::uint16_t book) { return registry.kind_of(book); });
        for (const auto& row : rows) {
            internalcross::LiabilityView view{.row = row};
            if (const auto* listed = instrument(row.instrument); listed != nullptr) {
                view.symbol = std::string{base_of(*listed).exchange_symbol.view()};
            }
            const auto cell = row.instrument.symbol().raw;
            const auto* gw = gateway_for(row.instrument);
            shm::PositionCell held{};
            if (gw != nullptr && cell < shm::k_position_cells && gw->ch.positions != nullptr &&
                gw->ch.positions->read(cell, held) != 0 && !held.report.net_qty.is_undef() && held.ts_pulled.ns != 0) {
                view.venue = held.report.net_qty.raw;
            }
            out.push_back(std::move(view));
        }
        return out;
    }

    // set once the registry exists, since the risk file may key a strategy by name
    std::function<std::string(std::uint16_t)> strategy_name;
    books::Registry* books{nullptr};
    // the one instance that may submit into another book; zero is none
    std::uint16_t executor{0};

    // set before anything can place an order
    bookrisk::RiskDesk* book_risk{nullptr};

    internalcross::CrossDesk* cross{nullptr};
    // fills and closes a submit made on the spot: their owners hear next pass, never inside their own submit call
    std::vector<OrderEvent> cross_events;

    [[nodiscard]] oms::RiskConfig base_config(std::uint16_t strategy_id) const {
        const auto name = strategy_name ? strategy_name(strategy_id) : std::string{};
        return config::to_risk_config(config::risk_for(risk_file_, name, strategy_id));
    }

    [[nodiscard]] oms::RiskConfig base_defaults() const { return config::to_risk_config(risk_file_.defaults); }

    [[nodiscard]] std::size_t open_count_of(std::uint16_t strategy_id) const noexcept {
        return store_->open_count_of(strategy_id);
    }

    portfolio::PositionBook positions;
    // cleared only by a restart
    oms::ReconGate recon_gate;
    std::unordered_map<std::uint64_t, portfolio::ReconVerdict> recon_seen;
    std::uint64_t recon_agreed{0};
    // Orders the venue had dropped while this process still held them open.
    std::uint64_t orders_closed_by_recon{0};

    oms::FeeLedger fees;
    std::uint64_t fees_agreed{0};
    std::uint64_t fees_disagreed{0};
    std::uint64_t fees_unmatched{0};
    std::uint64_t fees_unbooked{0};

    // compares the venue's fee to the one booked from the fill; books nothing
    void reconcile_fee(const LedgerEntry& charge, const InstrumentBase& base) {
        const auto checked = fees.reconcile(charge);
        switch (checked.verdict) {
        case oms::FeeVerdict::Agreed:
            ++fees_agreed;
            return;
        case oms::FeeVerdict::Disagreed:
            ++fees_disagreed;
            RLOG_ERROR("strategyd: FEE BREAK {} on {} — we booked {} e9, venue says {} e9",
                       charge.reference.view(),
                       base.exchange_symbol.view(),
                       checked.booked.raw,
                       charge.amount.raw);
            return;
        case oms::FeeVerdict::Unmatched:
            ++fees_unmatched;
            RLOG_WARN("strategyd: FEE {} on {} — venue charged {}.{:09} against no fill we hold",
                      charge.reference.view(),
                      base.exchange_symbol.view(),
                      charge.amount.raw / 1'000'000'000,
                      std::abs(charge.amount.raw) % 1'000'000'000);
            return;
        }
    }

    [[nodiscard]] oms::OrderStore& orders() noexcept { return *store_; }

    std::uint64_t recon_unknown{0};
    std::uint64_t recon_broken{0};

    // fill deltas kept to rewind the book to a snapshot's instant; see oms/recon.hpp
    std::unordered_map<std::uint64_t, std::vector<oms::BookedFill>> fills_after;

    void record_fill(InstrumentId instrument, Side side, QtyLots delta, UnixNanos ts) {
        fills_after[instrument.packed].push_back({.ts_ns = ts.ns, .lots = side == Side::Buy ? delta.raw : -delta.raw});
    }

    std::unordered_map<std::uint64_t, std::uint64_t> inflight_; // coid -> submit mono ns
    observ::Histogram submit_hist;

private:
public:
    void journal_pnl(const OrderEvent& event,
                     const Instrument& listed,
                     raikiri_journal::types::pnlKindEnum kind,
                     FixedE9 amount) noexcept {
        portfolio::append_pnl(*journal_, event, listed, kind, amount);
    }

    void journal_position(const portfolio::Position& position, const Instrument& listed) noexcept {
        portfolio::append_position(*journal_, position, listed);
    }

    // on the order path, so stack buffers only; limit and value keep the gate's raw units (lots or e9 quote), and a
    // reader gets the words from `why`
    void audit_refused(const OrderCmd& cmd,
                       std::string_view symbol,
                       const oms::RiskRefusal& refusal,
                       std::string_view why) noexcept {
        std::array<char, 24> actor{};
        const auto owner = cmd.client_order_id.strategy_id();
        const auto kind = books != nullptr ? books->kind_of(owner) : BookKind::Unknown;
        const auto named = kind == BookKind::Manual || owner == k_manual_strategy_id
                               ? fmt::format_to_n(actor.data(), actor.size(), "manual:{}", owner)
                           : kind == BookKind::Client ? fmt::format_to_n(actor.data(), actor.size(), "client:{}", owner)
                           : kind == BookKind::Hedge
                               ? fmt::format_to_n(actor.data(), actor.size(), "hedge:{}", owner)
                               : fmt::format_to_n(actor.data(), actor.size(), "strategy:{}", owner);
        std::array<char, 40> subject{};
        const auto about = fmt::format_to_n(subject.data(), subject.size(), "order:{}", cmd.client_order_id.raw);
        std::array<char, 32> notional{};
        const auto valued = refusal.order_notional.is_undef()
                                ? std::string_view{}
                                : std::string_view{format_e9(refusal.order_notional, notional.data(), notional.size())};
        std::array<char, 384> detail{};
        const auto wrote = fmt::format_to_n(
            detail.data(),
            detail.size(),
            R"({{"why":"{}","instrument":"{}","symbol":"{}","side":"{}","price_ticks":"{}","qty_lots":"{}",)"
            R"("limit":"{}","value":"{}","notional":"{}"}})",
            why,
            cmd.instrument.packed,
            symbol,
            to_string(cmd.side),
            cmd.price.raw,
            cmd.qty.raw,
            refusal.limit,
            refusal.value,
            valued);
        audit_event({actor.data(), named.size},
                    "order.refused",
                    {subject.data(), about.size},
                    {detail.data(), std::min(wrote.size, detail.size())});
    }

    [[nodiscard]] static constexpr std::int32_t refusal_code_of(oms::RiskReject why) noexcept {
        switch (why) {
        case oms::RiskReject::NakedShort:
            return k_refused_naked_short;
        case oms::RiskReject::ShortMarginUnknown:
            return k_refused_short_margin_unknown;
        case oms::RiskReject::ShortMargin:
            return k_refused_short_margin;
        case oms::RiskReject::ShortGreeks:
            return k_refused_short_greeks;
        default:
            return k_refused_risk;
        }
    }

    // set where a submit refuses, read by journal_refusal right after it on the same thread
    std::int32_t last_refusal_code_{0};

    [[nodiscard]] OrderEvent denied_of(const OrderCmd& cmd) const noexcept {
        OrderEvent event{};
        // denied, since the venue never saw it
        event.type = OrderEventType::Denied;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.price = cmd.price;
        event.qty = cmd.qty;
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = clock_->wall();
        event.ts_init = clock_->wall();
        return event;
    }

    // journal a desk order the risk gate refused so /v1/orders/{id} can answer
    void journal_refusal(const OrderCmd& cmd, strategy::RejectReason why, const OrderOrigin& origin) noexcept {
        journal_cmd(cmd, origin);
        auto denied = denied_of(cmd);
        // the gate's own reason where it gave one; a refusal from before the gate has only its kind
        denied.reject_code = last_refusal_code_ != 0                    ? last_refusal_code_
                             : why == strategy::RejectReason::Risk      ? k_refused_risk
                             : why == strategy::RejectReason::Duplicate ? k_refused_duplicate
                             : why == strategy::RejectReason::Capacity  ? k_refused_capacity
                                                                        : k_refused_venue_down;
        last_refusal_code_ = 0;
        journal_event(denied);
    }

    // a gateway journals what the venue pushed, often without our id; this copy carries it once attributed
    void journal_event(const OrderEvent& event) noexcept { oms::append_order_event(*journal_, event); }

private:
    void journal_cmd(const OrderCmd& cmd, const OrderOrigin& origin = {}) noexcept {
        const auto kind = books != nullptr ? books->kind_of(cmd.client_order_id.strategy_id()) : BookKind::Unknown;
        oms::append_order_cmd(*journal_, cmd, origin, kind);
    }

    struct Held {
        QtyLots position{QtyLots::from_raw(0)};
        QtyLots buy{QtyLots::from_raw(0)};
        QtyLots sell{QtyLots::from_raw(0)};
    };

    struct ShortGreeksState {
        optionrisk::BookGreeks greeks{};
        bool breached{false};
    };

    [[nodiscard]] const OptionInstrument* option_of(InstrumentId id) const noexcept {
        const auto* listed = instrument(id);
        return listed != nullptr ? std::get_if<OptionInstrument>(listed) : nullptr;
    }

    [[nodiscard]] std::optional<OptionGreeks> greeks_of(InstrumentId id) const noexcept {
        const auto idx = id.symbol().raw;
        const auto* gw = gateway_for(id);
        if (gw == nullptr || gw->ch.greeks == nullptr || idx >= shm::k_greeks_cells) {
            return std::nullopt;
        }
        OptionGreeks greeks{};
        if (gw->ch.greeks->read(idx, greeks) == 0 || greeks.instrument != id) {
            return std::nullopt;
        }
        return greeks;
    }

    [[nodiscard]] static FixedE9 plus(FixedE9 a, FixedE9 b) noexcept {
        return a.is_undef() || b.is_undef() ? FixedE9::undef() : a + b;
    }

    [[nodiscard]] static oms::ShortMarginRates short_rates(const OptionInstrument& option) noexcept {
        return oms::short_margin_rates(option.base.id.venue(), option.base.underlying.view());
    }

    // the venue's mark, else the book's mid
    [[nodiscard]] FixedE9 mark_of(const OptionInstrument& option, const std::optional<OptionGreeks>& greeks) const {
        if (greeks.has_value() && !greeks->mark_price.is_undef() && greeks->mark_price.raw > 0) {
            return greeks->mark_price;
        }
        const auto quote = bbo(option.base.id);
        if (!quote.has_value() || quote->bid.is_undef() || quote->ask.is_undef()) {
            return FixedE9::undef();
        }
        return option.base.ticks_to_price(PriceTicks::from_raw((quote->bid.raw + quote->ask.raw) / 2));
    }

    [[nodiscard]] FixedE9 available_margin(InstrumentId id, std::string_view asset, UnixNanos now) const {
        const auto* gw = gateway_for(id);
        if (gw == nullptr || gw->ch.balances == nullptr) {
            return FixedE9::undef();
        }
        shm::BalanceSnapshot snapshot{};
        if (gw->ch.balances->read(0, snapshot) == 0) {
            return FixedE9::undef();
        }
        return oms::margin_available(
            std::span{snapshot.rows}.first(snapshot.count), asset, snapshot.ts_pulled, now, k_margin_read_max_age_ns);
    }

    [[nodiscard]] oms::ShortLine short_line(const OptionInstrument& option, const Held& lots) const {
        const auto greeks = greeks_of(option.base.id);
        return {.option = &option,
                .position = lots.position,
                .working_sell = lots.sell,
                .index = greeks.has_value() ? greeks->index_price : FixedE9::undef(),
                .forward = greeks.has_value() ? greeks->underlying_price : FixedE9::undef(),
                .mark = mark_of(option, greeks)};
    }

    void fill_short_option(oms::Exposure& exposure,
                           InstrumentId id,
                           const std::unordered_map<std::uint64_t, Held>& held,
                           std::uint16_t strategy_id) const {
        const auto* option = id.market() == MarketType::Option ? option_of(id) : nullptr;
        if (option == nullptr) {
            return;
        }
        const auto line = short_line(*option, Held{});
        const auto asset = oms::settlement_asset(*option);
        auto& inputs = exposure.short_option;
        inputs.rates = short_rates(*option);
        inputs.index = line.index;
        inputs.forward = line.forward;
        inputs.mark = line.mark;
        inputs.margin_available = available_margin(id, asset, clock_->wall());
        if (const auto state = short_greeks_.find(strategy_id); state != short_greeks_.end()) {
            inputs.greeks_breached = state->second.breached;
        }
        for (const auto& [packed, lots] : held) {
            const InstrumentId other{packed};
            const auto* sold = other.market() == MarketType::Option ? option_of(other) : nullptr;
            if (sold == nullptr) {
                continue;
            }
            const auto figures = oms::short_figures(short_line(*sold, lots), short_rates(*sold));
            if (packed != id.packed) {
                inputs.naked_besides = plus(inputs.naked_besides, figures.naked_usd);
            }
            if (other.venue() == id.venue() && oms::settlement_asset(*sold) == asset) {
                inputs.margin_working = plus(inputs.margin_working, figures.margin_working);
            }
        }
    }

    [[nodiscard]] std::unordered_map<std::uint64_t, Held> held_by(std::uint16_t strategy_id) const {
        std::unordered_map<std::uint64_t, Held> out;
        for (const auto& position : positions.all()) {
            if (position.key().strategy_id == strategy_id && position.qty().raw != 0) {
                out[position.key().instrument.packed].position = position.qty();
            }
        }
        store_->for_each_open_of(strategy_id, [&](const oms::Order& open) {
            auto& held = out[open.instrument().packed];
            auto& side = open.side() == Side::Buy ? held.buy : held.sell;
            side = side + open.leaves();
        });
        return out;
    }

    // each instrument valued at its mid; undef when one has no quote, an old one or cannot be valued
    [[nodiscard]] FixedE9 open_notional_besides(const std::unordered_map<std::uint64_t, Held>& held,
                                                InstrumentId skip,
                                                UnixNanos now,
                                                const oms::RiskGate& gate) const {
        const auto max_age = gate.config().max_quote_age_ns;
        Notional128 total{};
        for (const auto& [packed, lots] : held) {
            const auto open = oms::open_lots(lots.position, lots.buy, lots.sell);
            if (packed == skip.packed || open == 0) {
                continue;
            }
            const InstrumentId id{packed};
            const auto* listed = instrument(id);
            const auto quote = bbo(id);
            if (listed == nullptr || !quote.has_value() || quote->bid.is_undef() || quote->ask.is_undef()) {
                return FixedE9::undef();
            }
            if (max_age > 0 && now.ns > quote->ts_init.ns && now.ns - quote->ts_init.ns > max_age) {
                return FixedE9::undef();
            }
            const auto mid = PriceTicks::from_raw((quote->bid.raw + quote->ask.raw) / 2);
            const auto value = oms::notional(*listed, mid, QtyLots::from_raw(open), underlying_price(id));
            if (!value.has_value()) {
                return FixedE9::undef();
            }
            total = total + *value;
        }
        return oms::to_e9(total);
    }

    std::vector<Gateway>* gateways_;
    oms::OrderStore* store_;
    config::RiskFile risk_file_;
    std::unordered_map<std::uint16_t, ShortGreeksState> short_greeks_;
    // a poll is about a minute apart at most, so two missed ones refuse a short opening
    static constexpr std::uint64_t k_margin_read_max_age_ns = 180'000'000'000ULL;
    oms::IdGen* ids_;
    journal::JournalWriter* journal_;
    audit::Sink* audit_;
    const Clock* clock_;
};

// working lots for one strategy; oms::working_lots nets the whole process
[[nodiscard]] oms::WorkingLots
working_lots_of(const oms::OrderStore& store, std::uint16_t strategy_id, InstrumentId instrument) noexcept {
    oms::WorkingLots out{};
    store.for_each_open([&](const oms::Order& order) {
        if (order.instrument() != instrument || order.client_order_id().strategy_id() != strategy_id) {
            return;
        }
        auto& side = order.side() == Side::Buy ? out.buy : out.sell;
        side = QtyLots::from_raw(side.raw + order.leaves().raw);
    });
    return out;
}

// one strategy's view of the kernel; it sees and cancels only its own orders
class InstanceContext final : public strategy::Context {
public:
    InstanceContext(KernelContext& kernel, std::uint16_t strategy_id, std::uint64_t ulid_seed, std::uint64_t first_seq)
        : kernel_{&kernel}, ids_{strategy_id, ulid_seed} {
        ids_.start_after(first_seq);
        if (const auto it = kernel.last_seq.find(strategy_id); it != kernel.last_seq.end()) {
            ids_.resume_after(it->second);
        }
    }

    [[nodiscard]] std::uint16_t strategy_id() const noexcept { return ids_.strategy_id(); }

    std::expected<ClientOrderId, strategy::RejectReason> submit(const strategy::NewOrder& order) noexcept override {
        return kernel_->submit_as(order, ids_.next_client_id());
    }

    std::expected<ClientOrderId, strategy::RejectReason>
    submit_child(const strategy::NewOrder& order, std::uint16_t book, const OrderOrigin& origin) noexcept override {
        if (strategy_id() != kernel_->executor) {
            return std::unexpected{strategy::RejectReason::Risk};
        }
        return kernel_->submit_as(order, ids_.next_child_id(book != 0 ? book : strategy_id()), origin);
    }

    bool cancel(ClientOrderId id) noexcept override { return mine(id) && kernel_->cancel(id); }

    bool amend(ClientOrderId id, PriceTicks price) noexcept override { return mine(id) && kernel_->amend(id, price); }

    void cancel_all(InstrumentId instrument) noexcept override { (void)pull(&instrument); }

    std::size_t pull_all() noexcept { return pull(nullptr); }

    std::optional<Bbo> bbo(InstrumentId instrument) const noexcept override { return kernel_->bbo(instrument); }

    const Instrument* instrument(InstrumentId id) const noexcept override { return kernel_->instrument(id); }

    const portfolio::Position* position(InstrumentId id) const noexcept override {
        return kernel_->positions.find({strategy_id(), id});
    }

    FixedE9 unrealized(InstrumentId id, PriceTicks mark) const noexcept override {
        const auto* held = position(id);
        const auto* listed = instrument(id);
        if (held == nullptr || listed == nullptr) {
            return FixedE9::from_raw(0);
        }
        return portfolio::unrealized_pnl(base_of(*listed), *held, mark);
    }

    std::optional<ClientOrderId> set_target(const portfolio::PortfolioTarget& target) noexcept override {
        auto held = QtyLots::from_raw(0);
        if (const auto* p = position(target.instrument); p != nullptr) {
            held = p->qty();
        }
        const auto working = working_lots_of(kernel_->orders(), strategy_id(), target.instrument);
        const auto move = portfolio::delta_to_target(target.qty, held, working.buy, working.sell);
        if (move.qty.raw == 0) {
            return std::nullopt;
        }
        const auto sent = submit({.instrument = target.instrument,
                                  .side = move.side,
                                  .type = OrderType::Market,
                                  .tif = TimeInForce::Ioc,
                                  .qty = move.qty});
        return sent.has_value() ? std::optional{*sent} : std::nullopt;
    }

    void set_timer(std::uint32_t timer_id, std::uint64_t delay_ns) noexcept override {
        const auto deadline = kernel_->clock().mono().ns + delay_ns;
        for (auto& timer : timers_) {
            if (timer.id == timer_id) {
                timer.deadline_ns = deadline; // re-arm replaces
                return;
            }
        }
        timers_.push_back({timer_id, deadline});
    }

    const Clock& clock() const noexcept override { return kernel_->clock(); }

    // disarm before firing; iterate by index because a callback may add timers
    template <typename F> void fire_due_timers(std::uint64_t now_ns, F&& fire) {
        const auto armed = timers_.size();
        for (std::size_t i = 0; i < armed; ++i) {
            if (timers_[i].deadline_ns == 0 || now_ns < timers_[i].deadline_ns) {
                continue;
            }
            const auto id = timers_[i].id;
            timers_[i].deadline_ns = 0;
            fire(id);
        }
    }

private:
    struct Timer {
        std::uint32_t id{0};
        std::uint64_t deadline_ns{0};
    };

    [[nodiscard]] bool mine(ClientOrderId id) const noexcept {
        return id.strategy_id() == strategy_id() || (id.is_child() && strategy_id() == kernel_->executor);
    }

    // collect before cancelling; cancel writes the journal and a ring
    std::size_t pull(const InstrumentId* only) noexcept {
        std::vector<ClientOrderId> mine;
        kernel_->orders().for_each_open([&](const oms::Order& order) {
            if (order.is_terminal() || order.client_order_id().strategy_id() != strategy_id()) {
                return;
            }
            if (only != nullptr && order.instrument() != *only) {
                return;
            }
            mine.push_back(order.client_order_id());
        });
        std::size_t pulled = 0;
        for (const auto id : mine) {
            pulled += kernel_->cancel(id) ? 1U : 0U;
        }
        return pulled;
    }

    KernelContext* kernel_;
    oms::IdGen ids_;
    std::vector<Timer> timers_;
};

// the book risk desk's view of this process
class KernelCross final : public internalcross::CrossKernel {
public:
    KernelCross(KernelContext& ctx,
                oms::OrderStore& store,
                books::Registry& registry,
                std::vector<internalcross::TouchGateway> gateways,
                const Clock& clock)
        : ctx_{&ctx}, store_{&store}, registry_{&registry}, touches_{std::move(gateways), clock} {}

    std::size_t touches(InstrumentId instrument, std::span<internalcross::Touch> out) override {
        return touches_.read(instrument, out);
    }

    internalcross::LegFacts
    facts(std::uint16_t book, InstrumentId instrument, ClientOrderId leaving_out) const override {
        internalcross::LegFacts out{};
        if (const auto* held = ctx_->positions.find({.strategy_id = book, .instrument = instrument}); held != nullptr) {
            out.position = held->qty();
        }
        const auto working = working_lots_of(*store_, book, instrument);
        out.working_buy = working.buy;
        out.working_sell = working.sell;
        if (const auto* own = store_->find(leaving_out); own != nullptr) {
            auto& side = own->side() == Side::Buy ? out.working_buy : out.working_sell;
            side = QtyLots::from_raw(side.raw - own->leaves().raw);
        }
        return out;
    }

    bool leg_allowed(const OrderCmd& leg, ClientOrderId standing) override { return ctx_->leg_allowed(leg, standing); }

    BookKind kind_of(std::uint16_t book) const override { return registry_->kind_of(book); }

    std::string name_of(std::uint16_t book) const override {
        const auto* found = registry_->find(book);
        return found != nullptr ? found->name : std::string{};
    }

    std::optional<std::uint16_t> hedge_book() const override {
        const auto* found = registry_->find(books::k_hedge);
        return found != nullptr ? std::optional{found->id} : std::nullopt;
    }

    void booked(const OrderEvent& leg, bool placed) override {
        ctx_->book_risk->filled(leg.client_order_id.strategy_id(), leg.instrument);
        if (placed) {
            ctx_->cross_events.push_back(leg);
        }
    }

private:
    KernelContext* ctx_;
    oms::OrderStore* store_;
    books::Registry* registry_;
    internalcross::ShmTouches touches_;
};

class KernelBooks final : public bookrisk::BookKernel {
public:
    KernelBooks(KernelContext& kernel, const std::vector<Gateway>& gateways, const strategy::Registry& live)
        : kernel_{&kernel}, gateways_{&gateways}, live_{&live} {}

    [[nodiscard]] oms::RiskConfig base_config(bookrisk::BookId book) const override {
        return kernel_->base_config(book);
    }

    [[nodiscard]] oms::RiskConfig base_defaults() const override { return kernel_->base_defaults(); }

    [[nodiscard]] std::string name_of(bookrisk::BookId book) const override {
        auto name = kernel_->strategy_name ? kernel_->strategy_name(book) : std::string{};
        if (name.empty()) {
            return book == k_manual_strategy_id ? std::string{"manual"} : "book " + std::to_string(book);
        }
        return name;
    }

    void known_books(std::vector<bookrisk::BookId>& out) const override {
        out.push_back(k_manual_strategy_id);
        for (const auto& view : live_->list()) {
            out.push_back(view.strategy_id);
        }
        for (const auto& position : kernel_->positions.all()) {
            out.push_back(position.key().strategy_id);
        }
        kernel_->orders().for_each_open(
            [&out](const oms::Order& open) { out.push_back(open.client_order_id().strategy_id()); });
    }

    std::size_t cancel_open(std::optional<bookrisk::BookId> book) override {
        std::vector<ClientOrderId> ids;
        kernel_->orders().for_each_open([&](const oms::Order& open) {
            if (!open.is_terminal() && (!book.has_value() || open.client_order_id().strategy_id() == *book)) {
                ids.push_back(open.client_order_id());
            }
        });
        std::size_t sent = 0;
        for (const auto id : ids) {
            sent += kernel_->cancel(id) ? 1U : 0U;
        }
        return sent;
    }

    [[nodiscard]] std::size_t open_orders(std::optional<bookrisk::BookId> book) const override {
        return book.has_value() ? kernel_->open_count_of(*book) : kernel_->orders().open_count();
    }

    [[nodiscard]] bookrisk::PnlReading pnl(bookrisk::BookId book) const override {
        bookrisk::PnlReading out{};
        const auto max_age = kernel_->book_risk->gate(book).config().max_quote_age_ns;
        const auto now = kernel_->clock().wall();
        for (const auto& position : kernel_->positions.all()) {
            if (position.key().strategy_id != book) {
                continue;
            }
            const auto id = position.key().instrument;
            const auto* listed = kernel_->instrument(id);
            if (listed == nullptr) {
                ++out.unvalued;
                continue;
            }
            bookrisk::Mark mark{.underlying = kernel_->underlying_price(id)};
            // a quote past the book's age limit marks nothing, as it would price nothing at the gate
            if (const auto quote = kernel_->bbo(id); quote.has_value() && !quote->bid.is_undef() &&
                                                     !quote->ask.is_undef() &&
                                                     (max_age == 0 || now.ns <= quote->ts_init.ns + max_age)) {
                mark.mid = PriceTicks::from_raw((quote->bid.raw + quote->ask.raw) / 2);
            }
            bookrisk::add_position(out, *listed, position, mark);
        }
        return out;
    }

    [[nodiscard]] bookrisk::BalanceReading balances() const override {
        std::vector<bookrisk::AccountChannels> accounts;
        accounts.reserve(gateways_->size());
        for (const auto& gw : *gateways_) {
            accounts.push_back({.venue = gw.account.venue, .channels = &gw.ch});
        }
        return bookrisk::read_balances(accounts);
    }

private:
    KernelContext* kernel_;
    const std::vector<Gateway>* gateways_;
    const strategy::Registry* live_;
};

} // namespace

constexpr std::uint64_t k_snapshot_max_age_ns = 90ULL * 1'000'000'000ULL;

constexpr std::uint64_t k_recon_read_interval_ns = 5ULL * 1'000'000'000ULL;
// a pass with no order-path work sleeps this long, so market data is read in batches at most this late: venues move
// in milliseconds, while a pass over every bbo cell costs tens of microseconds and back to back holds a whole core
constexpr auto k_idle_sleep = std::chrono::microseconds{500};

void journal_break(raikiri::journal::JournalWriter& journal,
                   raikiri::VenueId venue,
                   raikiri::InstrumentId instrument,
                   std::string_view symbol,
                   const raikiri::portfolio::ReconResult& result,
                   raikiri::UnixNanos now) {
    (void)raikiri::schema::append_msg<msgs::ReconBreakMsg>(journal, now.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.verdict(result.verdict == raikiri::portfolio::ReconVerdict::Unknown ? jt::reconVerdictEnum::Unknown
                                                                              : jt::reconVerdictEnum::Break);
        m.venue(static_cast<jt::venueIdEnum>(venue));
        m.instrument(jt::instrumentId{instrument.packed});
        m.symbol().assign_range(symbol);
        m.localQty(jt::qtyLots{result.local_qty.raw});
        m.venueQty(jt::qtyLots{result.venue_qty.raw});
        m.tsSeen(jt::timestampNs{now.ns});
    });
}

// a stale or half-written pull closes nothing, and orders newer than the pull are not judged
// the closes it made, for the strategies that own those orders
[[nodiscard]] std::vector<raikiri::OrderEvent> reconcile_open_orders(KernelContext& ctx,
                                                                     raikiri::AccountId account,
                                                                     const raikiri::shm::OpenOrderTable& table,
                                                                     raikiri::UnixNanos now) {
    using namespace raikiri;

    std::vector<OrderEvent> closed_events;
    const auto pull = shm::read_open_orders(table);
    if (!pull.has_value() || now.ns - pull->pulled.ns > k_snapshot_max_age_ns) {
        return closed_events;
    }
    std::vector<ExchangeClientOrderId> at_venue;
    at_venue.reserve(pull->rows.size());
    for (const auto& row : pull->rows) {
        at_venue.push_back(row.exchange_client_order_id);
    }
    for (const auto* order : oms::orders_gone(ctx.orders(), account, at_venue, pull->pulled)) {
        ++ctx.orders_closed_by_recon;
        RLOG_WARN("strategyd: RECON order {} is open here and gone at {} — closing",
                  order->exchange_client_order_id().view(),
                  to_string(account.venue));
        OrderEvent event{};
        event.type = OrderEventType::ReconClosed;
        event.instrument = order->instrument();
        event.side = order->side();
        event.client_order_id = order->client_order_id();
        event.exchange_client_order_id = order->exchange_client_order_id();
        event.exchange_order_id = order->exchange_order_id();
        event.qty = order->qty();
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = now;
        event.ts_init = now;
        // gone may mean filled: without this a fill pulled later matches no order and reaches nobody
        ctx.exec.closed().note(*order, order->leaves(), now);
        if (order->filled().raw == 0) {
            ctx.repeats.withdrawn(order->client_order_id());
        }
        const auto applied = ctx.orders().apply_event(event);
        // a restart must not bring it back
        ctx.journal_event(event);
        ctx.trail.event(event, applied.has_value() ? *applied : nullptr, now);
        closed_events.push_back(event);
    }
    return closed_events;
}

struct ReconSweep {
    bool confirmed_break{false};
    bool any_agreement{false};
    // a first disagreement: not a halt, but nothing yet to reopen on
    bool any_break{false};
    // the venue has reported positions at some point; one that never does can never agree
    bool any_written{false};
};

ReconSweep reconcile_against_venue(KernelContext& ctx,
                                   raikiri::VenueId venue,
                                   const raikiri::shm::CatalogueTable& catalogue,
                                   const raikiri::shm::PositionTable& table,
                                   raikiri::journal::JournalWriter& journal,
                                   const raikiri::oms::KernelGate& gate,
                                   raikiri::UnixNanos now) {
    using namespace raikiri;

    ReconSweep sweep;
    std::unordered_map<std::uint64_t, std::int64_t> book;
    for (const auto& held : ctx.positions.all()) {
        book[held.key().instrument.packed] += held.qty().raw;
    }
    for (auto& [packed, deltas] : ctx.fills_after) {
        oms::prune_fills(deltas, now, k_snapshot_max_age_ns);
    }

    for (const auto& listed : catalogue.all()) {
        const auto id = base_of(listed).id;
        const auto idx = id.symbol().raw;
        if (idx >= shm::k_position_cells) {
            continue;
        }
        shm::PositionCell cell{};
        const bool written = table.read(idx, cell) != 0;
        sweep.any_written = sweep.any_written || (written && cell.ts_pulled.ns != 0);
        // rewind fills the venue had not seen at the snapshot, using venue timestamps
        const auto boundary = cell.report.ts_venue.ns != 0 ? cell.report.ts_venue.ns : cell.ts_pulled.ns;
        auto local_raw = book.contains(id.packed) ? book[id.packed] : 0;
        if (const auto it = ctx.fills_after.find(id.packed); it != ctx.fills_after.end()) {
            local_raw = oms::book_as_of(local_raw, it->second, boundary);
        }
        const auto local = QtyLots::from_raw(local_raw);
        const bool answered = written && !cell.report.net_qty.is_undef() && cell.ts_pulled.ns != 0 &&
                              now.ns - cell.ts_pulled.ns <= k_snapshot_max_age_ns;
        if (!answered && local.raw == 0 && (!written || cell.report.net_qty.raw == 0)) {
            continue;
        }

        std::optional<PositionReport> report;
        if (answered && cell.report.net_qty.raw != 0) {
            report = cell.report;
        }
        const auto result = portfolio::reconcile_position(answered, report, local);
        switch (result.verdict) {
        case portfolio::ReconVerdict::Agree:
            ++ctx.recon_agreed;
            break;
        case portfolio::ReconVerdict::Unknown:
            ++ctx.recon_unknown;
            break;
        case portfolio::ReconVerdict::Break:
            ++ctx.recon_broken;
            break;
        }
        bool confirmed = false;
        if (result.verdict == portfolio::ReconVerdict::Break) {
            confirmed = ctx.recon_gate.on_break(id, boundary);
            sweep.confirmed_break = sweep.confirmed_break || confirmed;
            sweep.any_break = true;
        } else if (result.verdict == portfolio::ReconVerdict::Agree) {
            ctx.recon_gate.on_agree(id);
            sweep.any_agreement = sweep.any_agreement || gate.answers_doubt(cell.ts_pulled);
        }
        const auto seen = ctx.recon_seen.find(id.packed);
        const bool changed = seen == ctx.recon_seen.end() || seen->second != result.verdict;
        ctx.recon_seen[id.packed] = result.verdict;
        if (result.verdict == portfolio::ReconVerdict::Agree || (!changed && !confirmed)) {
            continue;
        }
        journal_break(journal, venue, id, base_of(listed).exchange_symbol.view(), result, now);
        if (confirmed) {
            RLOG_ERROR("strategyd: RECON {} (halted) {} local={} venue={} — {}",
                       portfolio::to_string(result.verdict),
                       base_of(listed).exchange_symbol.view(),
                       result.local_qty.raw,
                       result.venue_qty.raw,
                       result.detail);
            ctx.audit_event("strategyd",
                            "recon.halted",
                            base_of(listed).exchange_symbol.view(),
                            fmt::format(R"({{"verdict":"{}","instrument":"{}","local_lots":"{}","venue_lots":"{}"}})",
                                        portfolio::to_string(result.verdict),
                                        id.packed,
                                        result.local_qty.raw,
                                        result.venue_qty.raw));
        } else {
            RLOG_WARN("strategyd: RECON {} {} local={} venue={} — {}",
                      portfolio::to_string(result.verdict),
                      base_of(listed).exchange_symbol.view(),
                      result.local_qty.raw,
                      result.venue_qty.raw,
                      result.detail);
        }
    }
    return sweep;
}

// rebuild positions from the last PositionMsg per key in this process's journal, across every attached catalogue
struct Restoration {
    std::size_t restored{0};
    std::vector<std::string> unattached;
};

void write_funding_row(journal::JournalWriter& writer,
                       const LedgerEntry& charge,
                       const InstrumentBase& base,
                       std::uint16_t strategy_id,
                       FixedE9 amount) noexcept {
    if (amount.raw == 0) {
        return;
    }
    (void)schema::append_msg<msgs::PnlIncrementMsg>(writer, charge.settled_at.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.kind(jt::pnlKindEnum::Funding);
        m.settleSide(portfolio::settle_side(base.kind) == portfolio::SettleSide::Base ? jt::settleSideEnum::Base
                                                                                      : jt::settleSideEnum::Quote);
        m.strategyId(strategy_id);
        m.instrument(jt::instrumentId{base.id.packed});
        m.symbol().assign_range(base.exchange_symbol.view());
        // zero means no order
        m.clientOrderIdField(jt::clientOrderId{0});
        std::array<char, oms::k_max_funding_identity> buf{};
        m.execIdField().assign_range(oms::funding_identity(base.id, charge.settled_at, strategy_id, buf));
        m.amount(jt::fixedE9{amount.raw});
        m.tsEvent(jt::timestampNs{charge.settled_at.ns});
    });
}

void journal_funding(journal::JournalWriter& writer,
                     const portfolio::PositionBook& book,
                     const LedgerEntry& charge,
                     const InstrumentBase& base,
                     bool netted) noexcept {
    std::vector<oms::FundingHolder> holders;
    for (const auto& position : book.all()) {
        if (position.key().instrument == charge.instrument) {
            holders.push_back({.strategy_id = position.key().strategy_id, .lots = position.qty().raw});
        }
    }
    const auto shares = netted ? oms::split_funding_net(holders, charge.amount, k_manual_strategy_id)
                               : oms::split_funding(holders, charge.amount, k_manual_strategy_id);
    for (const auto& share : shares) {
        write_funding_row(writer, charge, base, share.strategy_id, share.amount);
    }
}

Restoration restore_book(portfolio::PositionBook& book,
                         std::span<const shm::CatalogueTable* const> catalogues,
                         const std::string& dir) {
    auto reader = journal::JournalReader::open(dir, "journal");
    if (!reader.has_value()) {
        return {}; // no journal yet: a first run, not a loss
    }

    // copied out because decoded messages point into the journal mapping
    struct Row {
        std::uint16_t strategy_id{0};
        InstrumentId instrument{};
        portfolio::Position::Restored state{};
    };

    // keyed on the pair; a shifted strategy id overlaps the venue and market bits
    std::map<std::pair<std::uint16_t, std::uint64_t>, Row> latest;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        if (oms::is_cross_frame(frame)) {
            // whole in one frame, so a crash cannot book one side of it
            const auto record = oms::decode_cross(frame);
            for (const auto* leg : {&record.buy, &record.sell}) {
                const auto strategy = leg->order.strategy_id();
                latest[{strategy, record.instrument.packed}] = {.strategy_id = strategy,
                                                                .instrument = record.instrument,
                                                                .state = {.qty = leg->net_qty,
                                                                          .cost = leg->cost,
                                                                          .realized = leg->realized,
                                                                          .fees = leg->fees,
                                                                          .fill_count = leg->fill_count,
                                                                          .ts_opened = leg->ts_opened,
                                                                          .ts_updated = record.ts}};
            }
            continue;
        }
        if (frame.schema_id != schema::message_id<msgs::PositionMsg>()) {
            continue;
        }
        const auto m = schema::view_msg<msgs::PositionMsg>(frame.payload);
        const InstrumentId id{m.instrument().value()};
        Row row{};
        row.strategy_id = m.strategyId().value();
        row.instrument = id;
        row.state = {.qty = QtyLots::from_raw(m.netQty().value()),
                     .cost = FixedE9::from_raw(m.cost().value()),
                     .realized = FixedE9::from_raw(m.realized().value()),
                     .fees = FixedE9::from_raw(m.fees().value()),
                     .fill_count = m.fillCount().value(),
                     .ts_opened = UnixNanos{m.tsOpened().value()},
                     .ts_updated = UnixNanos{m.tsUpdated().value()}};
        latest[{row.strategy_id, id.packed}] = row;
    }

    Restoration out{};
    std::set<std::string> unattached;
    for (const auto& [key, row] : latest) {
        if (row.state.qty.raw == 0 && row.state.realized.raw == 0) {
            continue; // a closed position carries nothing worth restoring
        }
        const bool listed = std::ranges::any_of(
            catalogues, [&](const auto* table) { return table != nullptr && table->find(row.instrument) != nullptr; });
        if (!listed) {
            std::array<char, 16> buf{};
            unattached.emplace(
                format_account({row.instrument.venue(), row.instrument.market()}, buf.data(), buf.size()));
            continue;
        }
        if (book.restore({.strategy_id = row.strategy_id, .instrument = row.instrument}, row.state)) {
            ++out.restored;
        }
    }
    out.unattached.assign(unattached.begin(), unattached.end());
    return out;
}

namespace {

struct Requested {
    std::string type;
    std::string name;
    std::string config;
    std::uint16_t strategy_id{0};
};

// the config is the whole remainder, colons included
[[nodiscard]] std::optional<Requested> parse_strategy_spec(std::string_view spec, std::uint16_t default_id) {
    Requested out{};
    out.strategy_id = default_id;
    const auto after_type = spec.find(':');
    out.type = std::string{spec.substr(0, after_type)};
    if (out.type.empty()) {
        return std::nullopt;
    }
    out.name = out.type;
    if (after_type == std::string_view::npos) {
        return out;
    }
    const auto rest = spec.substr(after_type + 1);
    const auto after_id = rest.find(':');
    if (const auto text = rest.substr(0, after_id); !text.empty()) {
        std::uint64_t value = 0;
        const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || stop != text.data() + text.size() || value > 0xFFFFULL) {
            return std::nullopt;
        }
        out.strategy_id = static_cast<std::uint16_t>(value);
    }
    if (after_id == std::string_view::npos) {
        return out;
    }
    const auto tail = rest.substr(after_id + 1);
    const auto after_name = tail.find(':');
    if (const auto text = tail.substr(0, after_name); !text.empty()) {
        out.name = std::string{text};
    }
    if (after_name != std::string_view::npos) {
        out.config = std::string{tail.substr(after_name + 1)};
    }
    return out;
}

[[nodiscard]] constexpr std::string_view reject_name(strategy::RejectReason why) noexcept {
    switch (why) {
    case strategy::RejectReason::Risk:
        return "risk gate";
    case strategy::RejectReason::Capacity:
        return "order store full";
    case strategy::RejectReason::VenueDown:
        return "no gateway for this instrument, or its command ring is full";
    case strategy::RejectReason::Duplicate:
        return "repeats an order just sent";
    }
    return "unknown";
}
} // namespace

constexpr std::string_view k_usage = R"(usage: strategyd [--gateway=<account>]... [--duration <sec>] [--strategy-id=<n>]
                 [--strategy <type>[:<id>[:<name>[:<config>]]]]... [--port <n>]
                 [--risk <path>] [--joe <strategy id>] [--seed-capital <dollars>]
                 [--audit-fsync never|batch|entry] [--audit-retain-days <n>] [--restore-wait <sec>]
)";

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            std::fputs(k_usage.data(), stdout);
            return 0;
        }
        if (arg == "--version") {
            std::printf("raikiri %.*s\n", static_cast<int>(raikiri::k_version.size()), raikiri::k_version.data());
            return 0;
        }
    }
    const raikiri::log::Scope logging{"strategyd"};
    // stdout holds the end-of-run report; line buffered
    (void)std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::uint64_t duration_s = 10;
    std::vector<std::string> wanted;
    // id 1 is k_manual_strategy_id, so the default is 2
    std::uint16_t strategy_id = 2;
    std::vector<std::string> requested;
    // zero serves no control surface
    std::uint16_t control_port = 0;
    std::string risk_path;
    std::string seed_capital;
    // zero leaves joe off
    std::uint16_t joe_id = 0;
    audit::SinkOptions audit_options{};
    // how long a restored book waits for a venue that has never reported a position before trading on it unconfirmed
    std::uint64_t restore_wait_s = 120;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--duration" && i + 1 < argc) {
            duration_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg.starts_with("--venue=") || arg.starts_with("--gateway=")) {
            wanted.emplace_back(arg.substr(arg.find('=') + 1));
        } else if (arg.starts_with("--strategy-id=")) {
            strategy_id = static_cast<std::uint16_t>(std::atoi(arg.substr(14).data()));
        } else if (arg.starts_with("--strategy=")) {
            requested.emplace_back(arg.substr(arg.find('=') + 1));
        } else if (arg == "--strategy" && i + 1 < argc) {
            requested.emplace_back(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            control_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--risk" && i + 1 < argc) {
            risk_path = argv[++i];
        } else if (arg == "--seed-capital" && i + 1 < argc) {
            seed_capital = argv[++i];
        } else if (arg == "--joe" && i + 1 < argc) {
            joe_id = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--audit-retain-days" && i + 1 < argc) {
            audit_options.retain_days = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--restore-wait" && i + 1 < argc) {
            restore_wait_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--audit-fsync" && i + 1 < argc) {
            const std::string_view policy{argv[++i]};
            if (policy != "never" && policy != "batch" && policy != "entry") {
                fmt::print(stderr, "strategyd: --audit-fsync takes never, batch or entry\n");
                return 2;
            }
            audit_options.fsync = policy == "never"   ? audit::FsyncPolicy::Never
                                  : policy == "batch" ? audit::FsyncPolicy::EachBatch
                                                      : audit::FsyncPolicy::EveryEntry;
        }
    }
    if (wanted.empty()) {
        wanted.emplace_back("sim");
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;

    // reserved so segment clients do not move
    std::vector<Gateway> gateways;
    gateways.reserve(wanted.size());
    for (const auto& name : wanted) {
        const auto account = account_from_string(name);
        if (account.venue == VenueId::Invalid) {
            fmt::print(stderr, "strategyd: unknown gateway {}\n", name);
            return 2;
        }
        for (const auto& existing : gateways) {
            if (existing.account == account) {
                fmt::print(stderr, "strategyd: {} named twice\n", name);
                return 2;
            }
        }
        // Retry: the gateway may still be coming up.
        const auto seg_path = shm::gateway_segment_path(account);
        std::expected<shm::SegmentClient, shm::SegError> client = std::unexpected{shm::SegError::MapFailed};
        for (int attempt = 0; attempt < 100; ++attempt) {
            client = shm::SegmentClient::attach(seg_path);
            if (client.has_value()) {
                break;
            }
            ::usleep(50'000);
        }
        if (!client.has_value()) {
            RLOG_ERROR("strategyd: cannot attach {}", seg_path);
            return 1;
        }
        auto channels = shm::open_gateway_channels(*client);
        if (!channels.has_value()) {
            RLOG_ERROR("strategyd: channel open failed on {} ({})", seg_path, static_cast<int>(channels.error()));
            return 1;
        }
        auto lease = shm::WriterLease::claim(seg_path, shm::LeaseRole::Strategy);
        if (!lease.has_value()) {
            RLOG_ERROR("strategyd: the command rings of {} are {} (pid {}); one strategyd per account",
                       seg_path,
                       shm::to_string(lease.error().error),
                       lease.error().holder_pid);
            return 1;
        }
        if (!client->register_reader(::getpid(), "strategyd").has_value()) {
            RLOG_WARN("strategyd: {} has no free reader slot; reading unlisted", seg_path);
        }
        gateways.emplace_back(account, name, std::move(*client), std::move(*lease), *channels);
    }

    const char* data_dir_env = std::getenv("RAIKIRI_DATA_DIR");
    const std::string data_dir = data_dir_env != nullptr ? data_dir_env : "raikiri-data";
    auto journal_writer = journal::JournalWriter::open(data_dir + "/journal/strategyd", "journal");
    if (!journal_writer.has_value()) {
        RLOG_ERROR("strategyd: journal open failed");
        return 1;
    }
    auto audit_sink = audit::FileSink::open(data_dir + "/audit/strategyd", audit_options, clock);
    if (!audit_sink.has_value()) {
        RLOG_ERROR("strategyd: audit log open failed: {}", audit::to_string(audit_sink.error()));
        return 1;
    }
    const auto process_detail = fmt::format(
        R"({{"pid":{},"gateways":{},"control_port":{},"joe":{}}})", ::getpid(), gateways.size(), control_port, joe_id);
    (*audit_sink)
        ->append({.ts = clock.wall(),
                  .actor = "strategyd",
                  .action = "process.start",
                  .subject = "strategyd",
                  .detail_json = process_detail});

    oms::OrderStore store{4096};
    // a named file must load; the committed one may be absent, which leaves no notional limit
    const auto default_risk = config::config_dir() + "/risk.yaml";
    const bool unlimited = risk_path.empty() && !std::filesystem::exists(default_risk);
    if (unlimited) {
        RLOG_WARN("strategyd: no {} — orders are not checked against any notional limit", default_risk);
    }
    auto risk = unlimited ? std::expected<config::RiskFile, std::string>{config::RiskFile{
                                .defaults = {.max_open_orders = 64, .max_price_band_bp = 500}}}
                          : config::load_risk(risk_path.empty() ? default_risk : risk_path);
    if (!risk.has_value()) {
        fmt::print(stderr, "strategyd: {}\n", risk.error());
        return 2;
    }
    const auto notify_webhook = risk->notify_webhook;
    const auto cross_file = risk->internal_cross;
    // used only for the exchange-side id; client order ids are minted per instance
    oms::IdGen ids{strategy_id, /*ulid_seed=*/0x7A10ULL};
    KernelContext ctx{gateways, store, std::move(*risk), ids, *journal_writer, **audit_sink, clock};

    std::size_t instrument_count = 0;
    for (const auto& gw : gateways) {
        instrument_count += gw.ch.catalogue->size();
    }
    if (instrument_count == 0) {
        RLOG_ERROR("strategyd: every attached gateway published an empty catalogue");
        return 1;
    }

    std::vector<const shm::CatalogueTable*> catalogues;
    catalogues.reserve(gateways.size());
    for (const auto& gw : gateways) {
        catalogues.push_back(gw.ch.catalogue);
    }
    const auto restoration = restore_book(ctx.positions, catalogues, data_dir + "/journal/strategyd");
    if (restoration.restored > 0) {
        RLOG_INFO("strategyd: restored {} position(s) from the journal", restoration.restored);
    }
    if (!restoration.unattached.empty()) {
        std::string accounts;
        for (const auto& name : restoration.unattached) {
            accounts += (accounts.empty() ? "" : ", ") + name;
        }
        RLOG_WARN("strategyd: the journal holds positions on {} and no attached gateway lists "
                  "them — this process is flat on those and will not reconcile them",
                  accounts);
    }

    // what was live before this process: open orders, executions already booked, ids already used. no window opens
    // here: every gateway holding anything stays shut until the venue has answered since this moment
    const std::string journal_dir = data_dir + "/journal/strategyd";
    oms::OrderReplay replay{store, ctx.exec};
    oms::replay_orders(journal_dir, "journal", replay);
    ctx.last_seq = replay.last_seqs();
    {
        std::vector<OrderEvent> orphans;
        store.for_each_open([&](const oms::Order& order) {
            if (ctx.gateway_for(order.instrument()) == nullptr) {
                OrderEvent gone{};
                gone.type = OrderEventType::ReconClosed;
                gone.client_order_id = order.client_order_id();
                gone.ts_init = clock.wall();
                orphans.push_back(gone);
            }
        });
        // no attached gateway can answer for these, and held open they would count against the book's limit
        for (const auto& gone : orphans) {
            (void)store.apply_event(gone);
        }
    }
    for (auto& gw : gateways) {
        bool holds = std::ranges::any_of(ctx.positions.all(), [&gw](const portfolio::Position& held) {
            const auto id = held.key().instrument;
            return held.qty().raw != 0 && id.venue() == gw.account.venue && id.market() == gw.account.market;
        });
        store.for_each_open([&](const oms::Order& order) {
            const auto id = order.instrument();
            holds = holds || (id.venue() == gw.account.venue && id.market() == gw.account.market);
        });
        if (replay.commands() != 0) {
            // what the venue said while nobody read; booked executions are refused by id
            gw.exec.rewind();
            gw.fills.rewind();
        }
        if (holds) {
            gw.restored = true;
            gw.kernel.on(oms::KernelEvent::Restored, clock.wall());
            RLOG_WARN("strategyd[{}]: restored from the journal — no new orders until the venue agrees", gw.name);
            ctx.audit_event("strategyd", "gateway.degraded", gw.name, R"({"why":"restored_from_journal"})");
        }
    }
    RLOG_INFO("strategyd: journal replay: {} commands, {} events, {} orders open, {} executions known",
              replay.commands(),
              replay.events(),
              store.open_count(),
              ctx.exec.seen().size());
    if (replay.dropped() != 0) {
        RLOG_ERROR("strategyd: journal replay: {} orders did not fit the store", replay.dropped());
    }

    // registered by a call; see raikiri/strategies/register.hpp
    (void)strategies::register_all();
    (void)strategy::register_type<Observer>("observer");

    books::FileBookStore book_store{books::default_path()};
    books::Registry book_registry{book_store, [&clock] { return clock.wall(); }};
    if (const auto loaded = book_registry.load(); !loaded.has_value()) {
        RLOG_ERROR("strategyd: {} — {}", books::default_path(), books::to_string(loaded.error()));
        return 1;
    }

    // deque so a strategy's context address stays stable
    std::deque<InstanceContext> contexts;
    std::stop_source shutdown;
    strategy::Registry live{{.context_for = [&contexts, &ctx, &clock](std::uint16_t id) -> strategy::Context* {
                                 for (auto& window : contexts) {
                                     if (window.strategy_id() == id) {
                                         // reuse the window a refused create already minted
                                         return &window;
                                     }
                                 }
                                 // seq from the wall clock, because postgres keys orders on the id across restarts
                                 contexts.emplace_back(ctx, id, 0x7A10ULL + id, clock.wall().ns / 1'000'000ULL);
                                 return &contexts.back();
                             },
                             .cancel_working = [&contexts](std::uint16_t id) -> std::size_t {
                                 for (auto& window : contexts) {
                                     if (window.strategy_id() == id) {
                                         return window.pull_all();
                                     }
                                 }
                                 return 0;
                             },
                             .wall = [&clock] { return clock.wall(); },
                             .admit = [&book_registry](const strategy::Spec& spec) -> std::expected<void, std::string> {
                                 const auto booked = book_registry.register_strategy(spec.strategy_id, spec.name);
                                 if (!booked.has_value()) {
                                     return std::unexpected{std::string{books::to_string(booked.error())}};
                                 }
                                 return {};
                             }}};
    ctx.books = &book_registry;
    ctx.strategy_name = [&live, &book_registry](std::uint16_t id) {
        if (const auto found = live.find(id); found.has_value()) {
            return found->name;
        }
        const auto* book = book_registry.find(id);
        return book != nullptr ? book->name : std::string{};
    };

    KernelBooks kernel_books{ctx, gateways, live};
    bookrisk::FileSettingsStore risk_store{data_dir + "/risk/settings.json"};
    bookrisk::LogNotifier log_notices;
    bookrisk::FanOutNotifier notices;
    notices.add(log_notices);
    config::DefaultSecretProvider notify_secrets;
    std::unique_ptr<bookrisk::WebhookNotifier> webhook;
    if (notify_webhook.has_value()) {
        auto made = bookrisk::make_webhook(*notify_webhook, notify_secrets, bookrisk::make_https_post());
        if (!made.has_value()) {
            fmt::print(stderr, "strategyd: {}\n", made.error());
            return 2;
        }
        webhook = std::move(*made);
        notices.add(*webhook);
    }
    bookrisk::RiskDesk risk_desk{kernel_books, risk_store, **audit_sink, notices, clock};
    if (const auto loaded = risk_desk.load(); !loaded.has_value()) {
        fmt::print(stderr, "strategyd: {}\n", loaded.error());
        return 2;
    }
    // sim has no money to protect, so its demos need no person to declare any
    const bool only_sim = !gateways.empty() && std::ranges::all_of(gateways, [](const Gateway& gw) {
        return gw.account.venue == VenueId::Sim;
    });
    if (seed_capital.empty() && only_sim) {
        seed_capital = "1000000";
    }
    if (!seed_capital.empty()) {
        const auto seeded = parse_decimal_e9(seed_capital);
        if (!seeded.has_value() || seeded->raw <= 0) {
            fmt::print(stderr, "strategyd: --seed-capital takes dollars above zero\n");
            return 2;
        }
        risk_desk.seed_capital(*seeded, "cli");
    }
    ctx.book_risk = &risk_desk;

    std::vector<internalcross::TouchGateway> touch_gateways;
    touch_gateways.reserve(gateways.size());
    for (const auto& gw : gateways) {
        touch_gateways.push_back({.account = gw.account, .channels = gw.ch});
    }
    KernelCross kernel_cross{ctx, store, book_registry, std::move(touch_gateways), clock};
    internalcross::JournalCrossLog cross_log{*journal_writer};
    internalcross::CrossDesk cross_desk{
        kernel_cross, cross_log, store, ctx.exec, ctx.positions, ctx.trail, **audit_sink};
    internalcross::FileSettingsStore cross_store{data_dir + "/risk/internal_cross.json"};
    internalcross::CrossControl cross_control{cross_desk, cross_store, **audit_sink, clock};
    {
        internalcross::Settings from_file{.enabled = cross_file.enabled,
                                          .max_spread_bp = cross_file.max_spread_bp,
                                          .max_quote_age_ms = cross_file.max_quote_age_ms,
                                          .hold_ms = cross_file.hold_ms,
                                          .against_desk = cross_file.against_desk};
        for (const auto& [name, book] : cross_file.books) {
            from_file.books[name] = {.enabled = book.enabled, .fee_bp = book.fee_bp, .instruments = book.instruments};
        }
        if (const auto loaded = cross_control.load(from_file); !loaded.has_value()) {
            fmt::print(stderr, "strategyd: {}\n", loaded.error());
            return 2;
        }
    }
    for (const auto& record : replay.crosses()) {
        cross_desk.restore(record);
    }
    // a wall-clock start as for any book, so a lost journal cannot mint a leg an earlier run used
    cross_desk.legs_start_after(std::max(replay.last_internal_leg(), clock.wall().ns / 1'000'000ULL));
    for (const auto& leg : replay.unechoed_legs()) {
        ctx.journal_event(leg);
    }
    ctx.cross = &cross_desk;
    ctx.cross_events.reserve(1024);
    if (cross_desk.settings().enabled) {
        RLOG_WARN("strategyd: internal crossing is ON");
    }

    // demo-maker only against sim; live gateways are observed
    if (requested.empty()) {
        const bool quoting = gateways.size() == 1 && gateways.front().account.venue == VenueId::Sim;
        requested.emplace_back(quoting ? "demo-maker" : "observer");
    }
    std::uint16_t next_id = strategy_id;
    for (const auto& spec : requested) {
        const auto parsed = parse_strategy_spec(spec, next_id);
        if (!parsed.has_value()) {
            fmt::print(stderr, "strategyd: cannot read --strategy {}\n", spec);
            return 2;
        }
        const auto made = live.create(
            {.name = parsed->name, .type = parsed->type, .config = parsed->config, .strategy_id = parsed->strategy_id});
        if (!made.has_value()) {
            fmt::print(stderr, "strategyd: --strategy {} refused — {}\n", spec, made.error().detail);
            return 2;
        }
        if (const auto started = live.start(parsed->strategy_id, shutdown.get_token()); !started.has_value()) {
            fmt::print(stderr, "strategyd: --strategy {} would not start — {}\n", spec, to_string(started.error()));
            return 2;
        }
        ctx.audit_event("cli",
                        "strategy.start",
                        fmt::format("strategy:{}", parsed->strategy_id),
                        fmt::format(R"({{"type":"{}"}})", parsed->type));
        // two instances under one id would share a position
        next_id = static_cast<std::uint16_t>(parsed->strategy_id + 1);
    }

    std::vector<joe::GatewayView> joe_views;
    joe_views.reserve(gateways.size());
    for (const auto& gw : gateways) {
        joe_views.push_back({.account = gw.account, .channels = gw.ch});
    }
    const joe::ShmMarket joe_market{std::move(joe_views), clock};
    joe::ContextSink joe_sink;
    const joe::CurveProfile joe_curves;
    std::unique_ptr<joe::Desk> joe_desk;
    joe::JournalParentLog joe_log{*journal_writer};
    if (joe_id != 0) {
        joe_desk = std::make_unique<joe::Desk>(
            joe_market,
            joe_sink,
            clock,
            joe_curves,
            joe::DeskConfig{.first_parent_id = joe::parent_id_seed(clock.wall()), .own_book = joe_id});
        ctx.executor = joe_id;
        live.set_executor(joe_id);
        const auto parent_records = joe::restore_desk(*joe_desk, journal_dir, "journal", joe_id);
        joe_desk->set_log(&joe_log);
        if (parent_records != 0) {
            RLOG_INFO(
                "strategyd: joe restored {} parent(s) from {} records", joe_desk->parents().size(), parent_records);
        }
        (void)strategy::factory().add("joe", [&joe_desk, &joe_sink](const strategy::Params&) -> strategy::MakeResult {
            return std::make_unique<joe::Runner>(*joe_desk, joe_sink);
        });
        const auto made = live.create({.name = "joe", .type = "joe", .strategy_id = joe_id});
        if (!made.has_value()) {
            fmt::print(stderr, "strategyd: --joe {} refused — {}\n", joe_id, made.error().detail);
            return 2;
        }
        if (const auto started = live.start(joe_id, shutdown.get_token()); !started.has_value()) {
            fmt::print(stderr, "strategyd: --joe {} would not start — {}\n", joe_id, to_string(started.error()));
            return 2;
        }
        ctx.audit_event("cli", "strategy.start", fmt::format("strategy:{}", joe_id), R"({"type":"joe"})");
    }

    std::string attached;
    for (const auto& gw : gateways) {
        attached += (attached.empty() ? "" : ", ") + gw.name;
    }
    std::string running;
    for (const auto& view : live.list()) {
        running += (running.empty() ? "" : ", ") + fmt::format("{}#{} ({})", view.name, view.strategy_id, view.type);
    }
    RLOG_INFO("strategyd: attached to {}, {} instruments, running {} for {}s",
              attached,
              instrument_count,
              running,
              duration_s);

    // loopback, no auth; polled only by the loop below because handlers run on_start and on_stop
    std::unique_ptr<http::Server> control;
    if (control_port != 0) {
        // the port carries no identity, so every command is the operator's
        control = std::make_unique<http::Server>(
            "127.0.0.1",
            control_port,
            audit::audited(
                [joe = joe::JoeRoutes{joe_desk.get(), &book_registry},
                 cross_routes = internalcross::CrossRoutes{
                     cross_control,
                     [&ctx, &book_registry] { return ctx.cross_liabilities(book_registry); },
                     [&kernel_cross](std::uint16_t book) { return kernel_cross.name_of(book); }},
                 book_risk = bookrisk::RiskRoutes{risk_desk},
                 short_books = optionrisk::ShortBookRoutes{[&] { return ctx.short_book_rows(clock.wall()); }},
                 control = strategy::make_control(live, strategy::factory(), ctx.positions, shutdown.get_token())](
                    const http::Request& req) {
                    if (auto answer = joe(req); answer.has_value()) {
                        return std::move(*answer);
                    }
                    if (auto answer = cross_routes(req); answer.has_value()) {
                        return std::move(*answer);
                    }
                    if (auto answer = book_risk(req); answer.has_value()) {
                        return *answer;
                    }
                    if (auto answer = short_books(req); answer.has_value()) {
                        return std::move(*answer);
                    }
                    return control(req);
                },
                **audit_sink,
                clock,
                "operator",
                control_command));
        if (!control->start()) {
            RLOG_ERROR("strategyd: cannot bind the control surface on 127.0.0.1:{}", control_port);
            return 1;
        }
        RLOG_INFO("strategyd: control on http://127.0.0.1:{} — admind forwards it with "
                  "--upstream <name>={}",
                  control->port(),
                  control->port());
    }

    constexpr std::uint64_t k_short_check_ns = 5'000'000'000ULL;
    UnixNanos last_short_check{};
    observ::Histogram tick_hist; // BBO ts_init -> callback dispatch
    observ::Histogram rt_hist;   // submit -> first venue verdict
    std::uint64_t exec_overruns = 0;
    std::uint64_t unattributed_fills = 0;
    std::uint64_t funding_overruns = 0;
    std::uint64_t funding_charges = 0;
    std::uint64_t pulled_fills = 0;
    std::uint64_t duplicate_pushes = 0;
    std::uint64_t late_fills = 0;
    std::uint64_t fill_overruns = 0;
    std::uint64_t fills = 0;
    std::uint64_t bad_transitions = 0;   // FSM refusals: must stay 0
    std::uint64_t position_refusals = 0; // position book full: must stay 0
    std::uint64_t funding_ticks = 0;
    std::uint64_t desk_intents = 0;  // hand-placed intents seen
    std::uint64_t desk_refusals = 0; // and how many never reached a venue
    // admind puts one desk cancel on every gateway's ring, the same command each time
    std::deque<std::pair<std::uint64_t, std::int64_t>> desk_cancels_seen;
    const auto audit_intent = [&ctx](const OrderCmd& intent, std::string_view why) {
        std::array<char, 40> subject{};
        const auto about = fmt::format_to_n(subject.data(), subject.size(), "order:{}", intent.client_order_id.raw);
        std::array<char, 64> detail{};
        const auto wrote = fmt::format_to_n(detail.data(), detail.size(), R"({{"why":"{}"}})", why);
        ctx.audit_event("desk", "intent.refused", {subject.data(), about.size}, {detail.data(), wrote.size});
    };
    const auto audit_gateway = [&ctx](const Gateway& gw, std::string_view action, std::string_view detail) {
        ctx.audit_event("strategyd", action, gw.name, detail);
    };

    for (auto& gw : gateways) {
        gw.recon_due = clock.mono().ns; // once at start, then on the interval
    }

    // a restarted gateway creates a new file, so watch the owner's heartbeat rather than the generation
    std::string gateway_gone;

    // wait for each owner's first heartbeat; a zero heartbeat reads as stale
    for (auto& gw : gateways) {
        constexpr int k_tries = 200; // 10 s at 50 ms
        int waited = 0;
        while (waited < k_tries && !gw.client.owner_alive(clock.mono().ns)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            ++waited;
        }
        if (waited == k_tries) {
            RLOG_ERROR("strategyd: {} never sent a heartbeat — it is not serving", gw.name);
            return 1;
        }
    }

    const auto t_end = clock.mono() + duration_s * 1'000'000'000ULL;
    const auto restore_deadline_ns = clock.mono().ns + (restore_wait_s * 1'000'000'000ULL);
    bool parents_resumed = false;
    std::vector<OrderEvent> crossing_events;
    crossing_events.reserve(1024);
    while (!g_stop.load(std::memory_order_relaxed) && clock.mono() < t_end) {
        if (const auto now = clock.wall(); now.ns - last_short_check.ns >= k_short_check_ns) {
            last_short_check = now;
            ctx.check_short_greeks(now);
        }
        // desk intents drain first, then verdicts on every gateway, then market data
        bool order_work = false;
        OrderCmd intent{};
        for (auto& gw : gateways) {
            while (gw.intents.try_pop(intent)) {
                order_work = true;
                desk_intents += 1;
                // intents may only use a manual or a client book
                if (!book_registry.takes_intents(intent.client_order_id)) {
                    desk_refusals += 1;
                    audit_intent(intent, "not_a_desk_book");
                    continue;
                }
                switch (intent.kind) {
                case CmdKind::Create: {
                    const strategy::NewOrder order{.instrument = intent.instrument,
                                                   .side = intent.side,
                                                   .type = intent.order_type,
                                                   .tif = intent.tif,
                                                   .flags = intent.flags,
                                                   .price = intent.price,
                                                   .qty = intent.qty};
                    const OrderOrigin origin{.external_ref = ExternalRef::from(intent.target_exchange_order_id.view())
                                                                 .value_or(ExternalRef{})};
                    const auto placed = ctx.submit_as(order, intent.client_order_id, origin);
                    // an intent resent under the id of an order still live must not mark that order denied
                    const bool still_live = store.find(intent.client_order_id) != nullptr;
                    if (!placed.has_value()) {
                        desk_refusals += 1;
                    }
                    if (!placed.has_value() && !still_live) {
                        // designated initializers in declaration order for gcc
                        ctx.journal_refusal(OrderCmd{.kind = CmdKind::Create,
                                                     .side = intent.side,
                                                     .order_type = intent.order_type,
                                                     .tif = intent.tif,
                                                     .flags = intent.flags,
                                                     .instrument = intent.instrument,
                                                     .client_order_id = intent.client_order_id,
                                                     .price = intent.price,
                                                     .qty = intent.qty,
                                                     .ts_created = clock.wall()},
                                            placed.error(),
                                            origin);
                    }
                    if (!placed.has_value()) {
                        RLOG_WARN("strategyd: desk order {} refused — {}",
                                  intent.client_order_id.raw,
                                  reject_name(placed.error()));
                    }
                    break;
                }
                case CmdKind::Cancel: {
                    const std::pair key{intent.client_order_id.raw, static_cast<std::int64_t>(intent.ts_created.ns)};
                    if (std::ranges::find(desk_cancels_seen, key) != desk_cancels_seen.end()) {
                        break;
                    }
                    desk_cancels_seen.push_back(key);
                    if (desk_cancels_seen.size() > 256) {
                        desk_cancels_seen.pop_front();
                    }
                    if (!ctx.cancel(intent.client_order_id)) {
                        desk_refusals += 1;
                        RLOG_WARN("strategyd: desk cancel {} refused — no such order here", intent.client_order_id.raw);
                        audit_intent(intent, "no_such_order");
                    }
                    break;
                }
                case CmdKind::Amend:
                    desk_refusals += 1;
                    audit_intent(intent, "amend_not_served");
                    break;
                }
            }
        }

        ctx.release_held();
        if (!ctx.cross_events.empty()) {
            order_work = true;
            crossing_events.swap(ctx.cross_events);
            for (const auto& event : crossing_events) {
                live.on_order_event(event);
            }
            crossing_events.clear();
        }

        shm::LedgerEntryRec charge_rec{};
        for (auto& gw : gateways) {
            for (;;) {
                const auto poll = gw.charges.poll(charge_rec);
                if (poll == shm::PollResult::Empty) {
                    break;
                }
                order_work = true;
                if (poll == shm::PollResult::Overrun) {
                    ++funding_overruns; // a lost charge is lost money: worth counting
                    continue;
                }
                const auto& charge = charge_rec.charge;
                const auto* instrument = gw.ch.catalogue->find(charge.instrument);
                if (instrument == nullptr) {
                    continue; // an id from a catalogue this process never read
                }
                // funding is booked here; fees were booked from the fill and are only compared
                if (charge.kind == LedgerKind::Funding) {
                    journal_funding(
                        *journal_writer, ctx.positions, charge, base_of(*instrument), cross_desk.has_crossed_or_may());
                    ++funding_charges;
                    continue;
                }
                ctx.reconcile_fee(charge, base_of(*instrument));
            }
        }

        // one path for pushed and pulled fills; by value because attribute writes our id onto it
        const auto handle_exec_event = [&](OrderEvent event, bool pulled) {
            const auto wall = clock.wall();
            const auto done = ctx.exec.apply(event, pulled, wall);
            // the pull admits before calling; a push whose execution a pull already booked is the same money
            if (done.repeat) {
                ++duplicate_pushes;
                return;
            }
            if (const auto it = ctx.inflight_.find(event.client_order_id.raw); it != ctx.inflight_.end()) {
                rt_hist.record(clock.mono().ns - it->second);
                ctx.inflight_.erase(it);
            }
            bad_transitions += done.refused ? 1U : 0U;
            if (event.client_order_id.raw != 0) {
                ctx.journal_event(event);
            }
            ctx.trail.event(event, done.order, wall);
            if (done.order != nullptr && done.order->is_terminal() && done.order->filled().raw == 0) {
                ctx.repeats.withdrawn(done.order->client_order_id());
            }
            if (event.type == OrderEventType::Filled) {
                ++fills;
                const auto delta = done.delta;
                if (done.late) {
                    ++late_fills;
                    RLOG_WARN("strategyd: late fill {} on a closed order books {} lots",
                              event.exchange_client_order_id.view(),
                              delta.raw);
                } else if (done.unmatched) {
                    ++unattributed_fills;
                    RLOG_ERROR("strategyd: FILL {} matched no order here", event.exchange_client_order_id.view());
                }
                if (done.order != nullptr) {
                    RLOG_INFO("strategyd: {} fill {} exec {} price {} fee {} qty {} leaves {} books {} lots",
                              pulled ? "pulled" : "pushed",
                              event.exchange_client_order_id.view(),
                              event.exec_id.view(),
                              event.price.raw,
                              event.fee.raw,
                              event.qty.raw,
                              event.leaves_qty.is_undef() ? -1 : event.leaves_qty.raw,
                              delta.raw);
                }
                const auto* listed = ctx.instrument(event.instrument);
                if (listed != nullptr && delta.raw > 0) {
                    ctx.record_fill(event.instrument, event.side, delta, event.ts_event);
                    risk_desk.filled(event.client_order_id.strategy_id(), event.instrument);
                    const auto applied = ctx.positions.on_fill(*listed, event, delta);
                    if (!applied.has_value()) {
                        ++position_refusals;
                    } else {
                        namespace jt = raikiri_journal::types;
                        // one increment per kind per exec_id
                        ctx.journal_pnl(event, *listed, jt::pnlKindEnum::Realized, applied->realized);
                        // fee from what the book took, negated to the ledger's positive-is-gain sign
                        const auto booked_fee = FixedE9::from_raw(-(applied->close_fee.raw + applied->open_fee.raw));
                        ctx.journal_pnl(event, *listed, jt::pnlKindEnum::Fee, booked_fee);
                        if (applied->unbooked_fee.raw == 0) {
                            ctx.fees.book(event.exec_id.view(), booked_fee, clock.wall());
                        } else {
                            ++ctx.fees_unbooked;
                            RLOG_WARN("strategyd: fee {} in {} on exec {} is left out of the position's sums",
                                      applied->unbooked_fee.raw,
                                      event.fee_asset.view(),
                                      event.exec_id.view());
                        }
                        if (const auto* held = ctx.positions.find(
                                {event.client_order_id.strategy_id(), event.instrument});
                            held != nullptr) {
                            ctx.journal_position(*held, *listed);
                        }
                    }
                }
            }
            live.on_order_event(event);
        };

        // pulled executions before the socket drain; duplicates are refused by id
        shm::FillRec fill_rec{};
        for (auto& gw : gateways) {
            for (;;) {
                const auto poll = gw.fills.poll(fill_rec);
                if (poll == shm::PollResult::Empty) {
                    break;
                }
                order_work = true;
                if (poll == shm::PollResult::Overrun) {
                    ++fill_overruns; // a lost execution is money the book never takes
                    continue;
                }
                const auto& fill = fill_rec.fill;
                if (gw.ch.catalogue->find(fill.instrument) == nullptr) {
                    continue; // an id from a catalogue this process never read
                }
                if (!ctx.exec.seen().admit(fill.exec_id.view(), clock.wall())) {
                    continue; // the same execution, offered again
                }
                ++pulled_fills;
                OrderEvent event{};
                event.type = OrderEventType::Filled;
                event.instrument = fill.instrument;
                event.side = fill.side;
                event.price = fill.price;
                event.qty = fill.qty;
                // a pull knows the execution, not the order's leaves
                event.leaves_qty = QtyLots::undef();
                event.fee = fill.fee;
                event.fee_asset = fill.fee_asset;
                event.liquidity = fill.liquidity;
                event.exec_id = fill.exec_id;
                event.exchange_order_id = fill.exchange_order_id;
                event.exchange_client_order_id = fill.exchange_client_order_id;
                event.ts_event = fill.ts_venue;
                event.ts_init = clock.wall();
                handle_exec_event(event, true);
            }
        }

        shm::ExecEventRec rec{};
        for (auto& gw : gateways) {
            for (;;) {
                const auto poll = gw.exec.poll(rec);
                if (poll == shm::PollResult::Empty) {
                    break;
                }
                order_work = true;
                if (poll == shm::PollResult::Overrun) {
                    ++exec_overruns;
                    // a dropped record may be a fill; stop adding risk on this gateway, cancels continue
                    if (gw.kernel.on(oms::KernelEvent::ExecOverrun, clock.wall())) {
                        RLOG_WARN("strategyd[{}]: execution record dropped — "
                                  "no new orders until reconciliation agrees",
                                  gw.name);
                        audit_gateway(gw, "gateway.degraded", R"({"why":"execution_record_dropped"})");
                    }
                    continue;
                }
                handle_exec_event(rec.event, false);
            }
        }
        shm::MdEvent md{};
        for (auto& gw : gateways) {
            while (gw.md.poll(md) == shm::PollResult::Item) {
                TradeTick trade{};
                trade.instrument = md.instrument;
                trade.price = md.price;
                trade.qty = md.qty;
                trade.aggressor = md.aggressor;
                trade.ts_event = md.ts_event;
                trade.ts_init = md.ts_init;
                live.on_trade(trade);
            }
        }
        for (auto& gw : gateways) {
            for (std::uint32_t sym = 0; sym < gw.bbo_seen.size(); ++sym) {
                const auto version = gw.ch.bbo->cells[sym].version_now();
                if (version == gw.bbo_seen[sym] || (version & 1U) != 0) {
                    continue;
                }
                Bbo bbo{};
                if (gw.ch.bbo->read(sym, bbo) != 0) {
                    gw.bbo_seen[sym] = version;
                    tick_hist.record(clock.wall().ns - bbo.ts_init.ns);
                    live.on_bbo(bbo);
                }
            }
            for (std::uint32_t sym = 0; sym < gw.funding_seen.size(); ++sym) {
                const auto version = gw.ch.funding->cells[sym].version_now();
                if (version == gw.funding_seen[sym] || (version & 1U) != 0) {
                    continue;
                }
                FundingUpdate funding{};
                if (gw.ch.funding->read(sym, funding) != 0) {
                    gw.funding_seen[sym] = version;
                    ++funding_ticks;
                    live.on_funding(funding);
                }
            }
        }
        risk_desk.tick();
        const auto now_ns = clock.mono().ns;
        for (auto& window : contexts) {
            window.fire_due_timers(now_ns, [&live, &window](std::uint32_t timer_id) {
                // through the registry so a timer on a torn-down instance lands nowhere
                live.on_timer(window.strategy_id(), timer_id);
            });
        }
        if (control != nullptr) {
            // polled on the loop thread that owns the order path
            (void)control->poll();
        }
        for (auto& gw : gateways) {
            // monotonic, the clock the heartbeat is stamped with
            if (!gw.client.owner_alive(now_ns)) {
                // this gateway only; a stall degrades and clears on the next agreement, a dead owner halts
                if (gw.client.owner_running()) {
                    if (gw.kernel.on(oms::KernelEvent::GatewayStalled, clock.wall())) {
                        RLOG_WARN("strategyd[{}]: gateway heartbeat stalled — no new orders until it agrees again",
                                  gw.name);
                        audit_gateway(gw, "gateway.degraded", R"({"why":"heartbeat_stalled"})");
                    }
                } else if (gw.kernel.on(oms::KernelEvent::GatewayLost, clock.wall())) {
                    RLOG_ERROR("strategyd[{}]: gateway stopped answering — halted", gw.name);
                    audit_gateway(gw, "gateway.halted", R"({"why":"owner_gone"})");
                }
                continue;
            }
            if (now_ns >= gw.recon_due) {
                // seven days is the widest ledger and trade-history window any venue reaches back over
                ctx.fees.prune(clock.wall(), 7ULL * 24 * 60 * 60 * 1'000'000'000ULL);
                ctx.exec.seen().prune(clock.wall(), 7ULL * 24 * 60 * 60 * 1'000'000'000ULL);
                // a pull window reaches back minutes, not hours
                ctx.exec.closed().prune(clock.wall(), oms::k_closed_order_window_ns);
                for (const auto& gone : reconcile_open_orders(ctx, gw.account, *gw.ch.open_orders, clock.wall())) {
                    live.on_order_event(gone);
                }
                const auto sweep = reconcile_against_venue(ctx,
                                                           gw.account.venue,
                                                           *gw.ch.catalogue,
                                                           *gw.ch.positions,
                                                           *journal_writer,
                                                           gw.kernel,
                                                           clock.wall());
                // a sweep that compared nothing neither halts nor clears
                if (sweep.confirmed_break) {
                    if (gw.kernel.on(oms::KernelEvent::ReconFailed, clock.wall())) {
                        RLOG_ERROR("strategyd[{}]: venue disagreed — halted until an operator "
                                   "resumes on a book that agrees",
                                   gw.name);
                        audit_gateway(gw, "gateway.halted", R"({"why":"venue_disagreed"})");
                    }
                } else if (sweep.any_agreement && !sweep.any_break) {
                    if (gw.kernel.on(oms::KernelEvent::ReconAgreed, clock.wall())) {
                        RLOG_INFO("strategyd[{}]: venue agrees — trading again", gw.name);
                        audit_gateway(gw, "gateway.running", R"({"why":"venue_agreed"})");
                    }
                    gw.restored = false;
                } else if (gw.restored && !sweep.any_written && now_ns >= restore_deadline_ns) {
                    gw.restored = false;
                    if (gw.kernel.on(oms::KernelEvent::ReconAgreed, clock.wall())) {
                        RLOG_ERROR("strategyd[{}]: this venue has never reported a position — trading on the journal's",
                                   gw.name);
                        audit_gateway(gw, "gateway.running", R"({"why":"venue_never_reports_positions"})");
                    }
                }
                gw.recon_due = now_ns + k_recon_read_interval_ns;
            }
        }
        if (!parents_resumed && std::ranges::all_of(gateways, [](const Gateway& gw) { return gw.kernel.may_open(); })) {
            parents_resumed = true;
            if (joe_desk != nullptr) {
                (void)joe_desk->resume_restored();
            }
        }
        if (std::ranges::all_of(gateways,
                                [](const Gateway& gw) { return gw.kernel.state() == oms::KernelState::Halted; })) {
            gateway_gone = gateways.empty() ? std::string{"none attached"} : gateways.front().name;
            break;
        }
        if (!order_work) {
            std::this_thread::sleep_for(k_idle_sleep);
        }
    }
    shutdown.request_stop();
    const auto torn_down = live.poll();
    if (control != nullptr) {
        control->stop();
    }
    journal_writer->flush();
    ctx.audit_event("strategyd",
                    "process.stop",
                    "strategyd",
                    gateway_gone.empty() ? std::string{"{}"} : R"({"why":"every_gateway_halted"})");
    if (!gateway_gone.empty()) {
        RLOG_ERROR("strategyd: every gateway is halted (first: {}) — nothing left to trade on, "
                   "stopping",
                   gateway_gone);
    }

    const auto tick = tick_hist.snapshot();
    const auto submit = ctx.submit_hist.snapshot();
    const auto rt = rt_hist.snapshot();
    fmt::print("\n===== raikiri: {}, {}s, single unpinned core =====\n", attached, duration_s);
    fmt::print("{:<22}{:>10}{:>12}{:>12}{:>12}{:>12}\n", "metric (ns)", "count", "p50", "p90", "p99", "max");
    const auto row = [](const char* name, const observ::HistogramSnapshot& s) {
        fmt::print("{:<22}{:>10}{:>12}{:>12}{:>12}{:>12}\n", name, s.count, s.p50_ns, s.p90_ns, s.p99_ns, s.max_ns);
    };
    row("tick-to-callback", tick);
    row("submit-call", submit);
    row("order-round-trip", rt);
    fmt::print("fills={}  funding={}  open-at-exit={}  exec-ring-overruns={}  fsm-refusals={}  "
               "position-refusals={}\n",
               fills,
               funding_ticks,
               store.open_count(),
               exec_overruns,
               bad_transitions,
               position_refusals);
    fmt::print("manual desk: intents={}  refused={}\n", desk_intents, desk_refusals);
    fmt::print("strategies: {} stopped at exit\n", torn_down);
    for (const auto& view : live.list()) {
        fmt::print("  {:<16}{:<14}{:<10}orders-pulled={}\n",
                   view.name,
                   view.type,
                   to_string(view.phase),
                   view.orders_cancelled_on_stop);
    }
    fmt::print("funding: charges-booked={}  ring-overruns={}\n", funding_charges, funding_overruns);
    fmt::print("pulled fills: applied={}  ring-overruns={}  remembered={}\n",
               pulled_fills,
               fill_overruns,
               ctx.exec.seen().size());
    fmt::print("fills-unattributed={}  duplicate-pushes={}  late-fills={}  duplicate-orders={}\n",
               unattributed_fills,
               duplicate_pushes,
               late_fills,
               ctx.duplicates_refused);
    fmt::print("recon: orders-closed={}\n", ctx.orders_closed_by_recon);
    fmt::print("fees: agreed={}  disagreed={}  unmatched={}  unbooked={}\n",
               ctx.fees_agreed,
               ctx.fees_disagreed,
               ctx.fees_unmatched,
               ctx.fees_unbooked);
    fmt::print("recon: agreed={}  unknown={}  broken={}  halted={}\n",
               ctx.recon_agreed,
               ctx.recon_unknown,
               ctx.recon_broken,
               ctx.recon_gate.halted_count());

    if (ctx.positions.size() > 0) {
        fmt::print(
            "\n{:<16}{:>12}{:>16}{:>16}{:>16}\n", "instrument", "net (lots)", "avg entry", "realized", "unrealized");
        char buf[32]{};
        for (const auto& position : ctx.positions.all()) {
            const auto* listed = ctx.instrument(position.key().instrument);
            if (listed == nullptr) {
                continue;
            }
            const auto& base = base_of(*listed);
            auto mark = PriceTicks::from_raw(0);
            // a one-sided book leaves a side undef, and a mid through it overflows the pnl into an abort
            if (const auto bbo = ctx.bbo(position.key().instrument);
                bbo.has_value() && !bbo->bid.is_undef() && !bbo->ask.is_undef()) {
                mark = PriceTicks::from_raw((bbo->bid.raw + bbo->ask.raw) / 2);
            }
            const auto fixed = [&buf](FixedE9 v) -> std::string {
                return v.is_undef() ? std::string{"-"} : std::string{format_e9(v, buf, sizeof(buf))};
            };
            fmt::print("{:<16}{:>12}{:>16}{:>16}{:>16}\n",
                       base.exchange_symbol.view(),
                       position.qty().raw,
                       fixed(position.avg_entry_price(base)),
                       fixed(position.net_realized()),
                       mark.raw == 0 ? std::string{"-"} : fixed(portfolio::unrealized_pnl(base, position, mark)));
        }
    }
    return gateway_gone.empty() ? 0 : 1;
}
