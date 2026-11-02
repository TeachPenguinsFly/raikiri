// strategyd: strategy kernel; attaches gateways, runs strategies and serves a loopback control port
// single thread: rings, timers and the control surface are polled in one loop

#include <raikiri/audit/audited.hpp>
#include <raikiri/audit/file_sink.hpp>
#include <raikiri/common/version.hpp>
#include <raikiri/config/risk.hpp>
#include <raikiri/config/settings.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/joe/desk.hpp>
#include <raikiri/joe/routes.hpp>
#include <raikiri/joe/runner.hpp>
#include <raikiri/joe/shm_market.hpp>
#include <raikiri/journal/internal.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/observ/histogram.hpp>
#include <raikiri/oms/attribution.hpp>
#include <raikiri/oms/fee_ledger.hpp>
#include <raikiri/oms/funding_identity.hpp>
#include <raikiri/oms/funding_split.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/kernel_gate.hpp>
#include <raikiri/oms/order_store.hpp>
#include <raikiri/oms/recon.hpp>
#include <raikiri/oms/risk.hpp>
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
        : gateways_{&gateways}, store_{&store}, risk_file_{std::move(risk)}, ids_{&ids}, journal_{&journal},
          audit_{&audit}, clock_{&clock} {}

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
    std::expected<ClientOrderId, strategy::RejectReason> submit_as(const strategy::NewOrder& order,
                                                                   ClientOrderId id) noexcept {
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
        const auto& gate = gate_for(id.strategy_id());
        const auto held = held_by(id.strategy_id());
        oms::Exposure exposure{.reconciliation_break = recon_gate.halted(order.instrument) || !gw->kernel.may_open(),
                               .open_orders = store_->open_count(),
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
        }
        exposure.underlying_price = underlying_price(order.instrument);
        if (!gate.config().max_strategy_notional.is_undef()) {
            exposure.strategy_open_notional = open_notional_besides(held, order.instrument);
        }
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
            return std::unexpected{strategy::RejectReason::Risk};
        }
        if (!store_->create(cmd, cmd.exchange_client_order_id).has_value()) {
            audit_refused(cmd, {}, oms::RiskRefusal{}, "order_store_full");
            return std::unexpected{strategy::RejectReason::Capacity};
        }
        // journal the intent before the wire
        journal_cmd(cmd);
        if (!gw->creates.try_push(cmd).has_value()) {
            audit_refused(cmd, {}, oms::RiskRefusal{}, "ring_full");
            return std::unexpected{strategy::RejectReason::VenueDown};
        }
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

        inflight_[cmd.client_order_id.raw] = t0.ns;
        submit_hist.record(clock_->mono().ns - t0.ns);
        return cmd.client_order_id;
    }

    bool cancel(ClientOrderId id) noexcept {
        const auto* order = store_->find(id);
        if (order == nullptr) {
            return false;
        }
        OrderCmd cmd{};
        cmd.kind = CmdKind::Cancel;
        cmd.instrument = order->instrument();
        cmd.client_order_id = id;
        cmd.exchange_client_order_id = order->exchange_client_order_id();
        cmd.target_exchange_order_id = order->exchange_order_id();
        cmd.ts_created = clock_->wall();
        auto* gw = gateway_for(cmd.instrument);
        if (gw == nullptr) {
            return false;
        }
        journal_cmd(cmd);
        return gw->cancels.try_push(cmd).has_value();
    }

    bool amend(ClientOrderId id, PriceTicks price) noexcept {
        const auto* order = store_->find(id);
        if (order == nullptr || order->is_terminal()) {
            return false;
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
        cmd.qty = order->qty();
        cmd.ts_created = clock_->wall();
        // the order's own size is already working, so it is not counted twice
        const auto held = held_by(id.strategy_id());
        oms::Exposure exposure{.reconciliation_break = recon_gate.halted(cmd.instrument) || !gw->kernel.may_open(),
                               .open_orders = store_->open_count() - 1,
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
        }
        exposure.underlying_price = underlying_price(cmd.instrument);
        const auto& gate = gate_for(id.strategy_id());
        if (!gate.config().max_strategy_notional.is_undef()) {
            exposure.strategy_open_notional = open_notional_besides(held, cmd.instrument);
        }
        if (const auto allowed = gate.check(cmd, exposure); !allowed.has_value()) {
            RLOG_WARN("strategyd: risk refused amend coid={} — {}", id.raw, oms::describe(allowed.error()));
            return false;
        }
        journal_cmd(cmd);
        if (!gw->creates.try_push(cmd).has_value()) {
            return false;
        }
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
        (void)store_->apply_event(pending);
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

    [[nodiscard]] const Clock& clock() const noexcept { return *clock_; }

    // set once the registry exists, since the risk file may key a strategy by name
    std::function<std::string(std::uint16_t)> strategy_name;

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

    oms::SeenFills seen_fills;

    std::uint64_t recon_unknown{0};
    std::uint64_t recon_broken{0};

    // fill deltas kept to rewind the book to a snapshot's instant; see oms/recon.hpp
    std::unordered_map<std::uint64_t, std::vector<oms::BookedFill>> fills_after;

    void record_fill(InstrumentId instrument, Side side, QtyLots delta, UnixNanos ts) {
        fills_after[instrument.packed].push_back({.ts_ns = ts.ns, .lots = side == Side::Buy ? delta.raw : -delta.raw});
    }

    std::unordered_map<std::uint64_t, std::uint64_t> inflight_; // coid -> submit mono ns
    observ::Histogram submit_hist;

    // an order closed with lots unfilled, so a fill that raced its cancel still books to its strategy
    struct ClosedOrder {
        ClientOrderId id{};
        QtyLots left{};
        std::uint64_t at_ns{0};
    };

    std::unordered_map<std::string, ClosedOrder> closed_orders; // by exchange client order id

private:
public:
    void journal_pnl(const OrderEvent& event,
                     const Instrument& listed,
                     raikiri_journal::types::pnlKindEnum kind,
                     FixedE9 amount) noexcept {
        if (amount.raw == 0 || amount.is_undef()) {
            return; // an increment of nothing is not a fact worth keeping
        }
        const auto& base = base_of(listed);
        (void)schema::append_msg<msgs::PnlIncrementMsg>(*journal_, event.ts_event.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.kind(kind);
            m.settleSide(portfolio::settle_side(listed) == portfolio::SettleSide::Base ? jt::settleSideEnum::Base
                                                                                       : jt::settleSideEnum::Quote);
            m.strategyId(event.client_order_id.strategy_id());
            m.instrument(jt::instrumentId{event.instrument.packed});
            m.symbol().assign_range(base.exchange_symbol.view());
            m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
            m.execIdField().assign_range(
                fill_identity(event.client_order_id, event.leaves_qty, event.exec_id.view()).view());
            m.amount(jt::fixedE9{amount.raw});
            m.tsEvent(jt::timestampNs{event.ts_event.ns});
        });
    }

    void journal_position(const portfolio::Position& position, const Instrument& listed) noexcept {
        const auto& base = base_of(listed);
        (void)schema::append_msg<msgs::PositionMsg>(*journal_, position.ts_updated().ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.strategyId(position.key().strategy_id);
            m.settleSide(portfolio::settle_side(listed) == portfolio::SettleSide::Base ? jt::settleSideEnum::Base
                                                                                       : jt::settleSideEnum::Quote);
            m.venue(static_cast<jt::venueIdEnum>(position.key().instrument.venue()));
            m.instrument(jt::instrumentId{position.key().instrument.packed});
            m.symbol().assign_range(base.exchange_symbol.view());
            m.netQty(jt::qtyLots{position.qty().raw});
            // store cost; the average is derived
            m.cost(jt::fixedE9{position.cost().raw});
            m.realized(jt::fixedE9{position.realized().raw});
            m.fees(jt::fixedE9{position.fees().raw});
            m.fillCount(position.fill_count());
            m.tsOpened(jt::timestampNs{position.ts_opened().ns});
            m.tsUpdated(jt::timestampNs{position.ts_updated().ns});
        });
    }

    // on the order path, so stack buffers only; limit and value keep the gate's raw units (lots or e9 quote), and a
    // reader gets the words from `why`
    void audit_refused(const OrderCmd& cmd,
                       std::string_view symbol,
                       const oms::RiskRefusal& refusal,
                       std::string_view why) noexcept {
        std::array<char, 24> actor{};
        const auto owner = cmd.client_order_id.strategy_id();
        const auto named = owner == k_manual_strategy_id
                               ? fmt::format_to_n(actor.data(), actor.size(), "desk")
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

    // journal a desk order the risk gate refused so /v1/orders/{id} can answer
    void journal_refusal(const OrderCmd& cmd, strategy::RejectReason why) noexcept {
        journal_cmd(cmd);
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
        journal_event(event);
        (void)why;
    }

    // a gateway journals what the venue pushed, often without our id; this copy carries it once attributed
    void journal_event(const OrderEvent& event) noexcept {
        (void)schema::append_msg<msgs::OrderEventMsg>(*journal_, event.ts_init.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.eventType(static_cast<jt::orderEventTypeEnum>(event.type));
            m.side(static_cast<jt::sideEnum>(event.side));
            m.orderType(static_cast<jt::orderTypeEnum>(event.order_type));
            m.tif(static_cast<jt::tifEnum>(event.tif));
            m.flags(static_cast<std::uint8_t>(event.flags));
            m.liquidity(static_cast<jt::liquidityEnum>(event.liquidity));
            m.instrument(jt::instrumentId{event.instrument.packed});
            m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
            m.exchangeClientOrderIdField().assign_range(event.exchange_client_order_id.view());
            m.exchangeOrderIdField().assign_range(event.exchange_order_id.view());
            m.execIdField().assign_range(event.exec_id.view());
            m.price(jt::priceTicks{event.price.raw});
            m.qty(jt::qtyLots{event.qty.raw});
            m.leavesQty(jt::qtyLots{event.leaves_qty.raw});
            m.fee(jt::fixedE9{event.fee.raw});
            m.tsEvent(jt::timestampNs{event.ts_event.ns});
            m.tsInit(jt::timestampNs{event.ts_init.ns});
            m.rejectClass(static_cast<jt::errorClassEnum>(event.reject_class));
            m.rejectCode(jt::venueCode{event.reject_code});
            m.feeAsset().assign_range(event.fee_asset.view());
        });
    }

private:
    void journal_cmd(const OrderCmd& cmd) noexcept {
        (void)schema::append_msg<msgs::OrderCmdMsg>(*journal_, cmd.ts_created.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.kind(static_cast<jt::cmdKindEnum>(cmd.kind));
            m.side(static_cast<jt::sideEnum>(cmd.side));
            m.orderType(static_cast<jt::orderTypeEnum>(cmd.order_type));
            m.tif(static_cast<jt::tifEnum>(cmd.tif));
            m.flags(static_cast<std::uint8_t>(cmd.flags));
            m.instrument(jt::instrumentId{cmd.instrument.packed});
            m.clientOrderIdField(jt::clientOrderId{cmd.client_order_id.raw});
            m.exchangeClientOrderIdField().assign_range(cmd.exchange_client_order_id.view());
            m.exchangeOrderIdField().assign_range(cmd.target_exchange_order_id.view());
            m.price(jt::priceTicks{cmd.price.raw});
            m.qty(jt::qtyLots{cmd.qty.raw});
            m.tsCreated(jt::timestampNs{cmd.ts_created.ns});
        });
    }

    struct Held {
        QtyLots position{QtyLots::from_raw(0)};
        QtyLots buy{QtyLots::from_raw(0)};
        QtyLots sell{QtyLots::from_raw(0)};
    };

    // read from the risk file once per strategy, under its name or id
    const oms::RiskGate& gate_for(std::uint16_t strategy_id) {
        if (const auto it = gates_.find(strategy_id); it != gates_.end()) {
            return it->second;
        }
        const auto name = strategy_name ? strategy_name(strategy_id) : std::string{};
        const oms::RiskGate gate{config::to_risk_config(config::risk_for(risk_file_, name, strategy_id))};
        return gates_.emplace(strategy_id, gate).first->second;
    }

    [[nodiscard]] std::unordered_map<std::uint64_t, Held> held_by(std::uint16_t strategy_id) const {
        std::unordered_map<std::uint64_t, Held> out;
        for (const auto& position : positions.all()) {
            if (position.key().strategy_id == strategy_id && position.qty().raw != 0) {
                out[position.key().instrument.packed].position = position.qty();
            }
        }
        store_->for_each_open([&](const oms::Order& open) {
            if (open.client_order_id().strategy_id() != strategy_id) {
                return;
            }
            auto& held = out[open.instrument().packed];
            auto& side = open.side() == Side::Buy ? held.buy : held.sell;
            side = side + open.leaves();
        });
        return out;
    }

    // each instrument valued at its mid; undef when one has no quote or cannot be valued
    [[nodiscard]] FixedE9 open_notional_besides(const std::unordered_map<std::uint64_t, Held>& held,
                                                InstrumentId skip) const {
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
    std::unordered_map<std::uint16_t, oms::RiskGate> gates_;
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
    InstanceContext(KernelContext& kernel, std::uint16_t strategy_id, std::uint64_t ulid_seed)
        : kernel_{&kernel}, ids_{strategy_id, ulid_seed} {}

    [[nodiscard]] std::uint16_t strategy_id() const noexcept { return ids_.strategy_id(); }

    std::expected<ClientOrderId, strategy::RejectReason> submit(const strategy::NewOrder& order) noexcept override {
        return kernel_->submit_as(order, ids_.next_client_id());
    }

    bool cancel(ClientOrderId id) noexcept override { return id.strategy_id() == strategy_id() && kernel_->cancel(id); }

    bool amend(ClientOrderId id, PriceTicks price) noexcept override {
        return id.strategy_id() == strategy_id() && kernel_->amend(id, price);
    }

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

// a stale table closes nothing, and orders newer than the pull are not judged
// the closes it made, for the strategies that own those orders
[[nodiscard]] std::vector<raikiri::OrderEvent> reconcile_open_orders(KernelContext& ctx,
                                                                     raikiri::VenueId venue,
                                                                     const raikiri::shm::OpenOrderTable& table,
                                                                     raikiri::journal::JournalWriter& journal,
                                                                     raikiri::UnixNanos now) {
    using namespace raikiri;

    // owned strings: `cell` is rebuilt each iteration
    std::unordered_set<std::string> at_venue;
    UnixNanos pulled{};
    for (std::size_t idx = 0; idx < shm::k_open_order_cells; ++idx) {
        shm::OpenOrderCell cell{};
        if (table.read(idx, cell) == 0 || cell.ts_pulled.ns == 0) {
            continue;
        }
        if (now.ns - cell.ts_pulled.ns > k_snapshot_max_age_ns) {
            continue; // last pass has aged out
        }
        pulled = UnixNanos{std::max(pulled.ns, cell.ts_pulled.ns)};
        at_venue.emplace(cell.exchange_client_order_id.view());
    }
    std::vector<OrderEvent> closed_events;
    if (pulled.ns == 0) {
        return closed_events; // the venue said nothing recent; this is Unknown, not empty
    }

    std::vector<const oms::Order*> closed;
    ctx.orders().for_each_open([&](const oms::Order& order) {
        if (order.is_terminal() || order.instrument().venue() != venue) {
            return;
        }
        if (at_venue.contains(std::string{order.exchange_client_order_id().view()})) {
            return;
        }
        if (order.ts_created().ns == 0 || order.ts_created().ns > pulled.ns) {
            return;
        }
        closed.push_back(&order);
    });

    for (const auto* order : closed) {
        ++ctx.orders_closed_by_recon;
        RLOG_WARN("strategyd: RECON order {} is open here and gone at {} — closing",
                  order->exchange_client_order_id().view(),
                  to_string(venue));
        OrderEvent event{};
        event.type = OrderEventType::ReconClosed;
        event.instrument = order->instrument();
        event.side = order->side();
        event.client_order_id = order->client_order_id();
        event.exchange_client_order_id = order->exchange_client_order_id();
        event.qty = order->qty();
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = now;
        event.ts_init = now;
        // gone may mean filled: without this a fill pulled later matches no order and reaches nobody
        if (const auto left = order->leaves(); !left.is_undef() && left.raw > 0) {
            ctx.closed_orders[std::string{order->exchange_client_order_id().view()}] = {
                .id = order->client_order_id(), .left = left, .at_ns = now.ns};
        }
        (void)ctx.orders().apply_event(event);
        (void)journal;
        closed_events.push_back(event);
    }
    return closed_events;
}

struct ReconSweep {
    bool confirmed_break{false};
    bool any_agreement{false};
};

ReconSweep reconcile_against_venue(KernelContext& ctx,
                                   raikiri::VenueId venue,
                                   const raikiri::shm::CatalogueTable& catalogue,
                                   const raikiri::shm::PositionTable& table,
                                   raikiri::journal::JournalWriter& journal,
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
        } else if (result.verdict == portfolio::ReconVerdict::Agree) {
            ctx.recon_gate.on_agree(id);
            sweep.any_agreement = true;
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
                     const InstrumentBase& base) noexcept {
    std::vector<oms::FundingHolder> holders;
    for (const auto& position : book.all()) {
        if (position.key().instrument == charge.instrument) {
            holders.push_back({.strategy_id = position.key().strategy_id, .lots = position.qty().raw});
        }
    }
    for (const auto& share : oms::split_funding(holders, charge.amount, k_manual_strategy_id)) {
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
    }
    return "unknown";
}
} // namespace

constexpr std::string_view k_usage = R"(usage: strategyd [--gateway=<account>]... [--duration <sec>] [--strategy-id=<n>]
                 [--strategy <type>[:<id>[:<name>[:<config>]]]]... [--port <n>]
                 [--risk <path>] [--joe <strategy id>]
                 [--audit-fsync never|batch|entry]
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
    // zero leaves joe off
    std::uint16_t joe_id = 0;
    audit::SinkOptions audit_options{};
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
        } else if (arg == "--joe" && i + 1 < argc) {
            joe_id = static_cast<std::uint16_t>(std::atoi(argv[++i]));
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

    // registered by a call; see raikiri/strategies/register.hpp
    (void)strategies::register_all();
    (void)strategy::register_type<Observer>("observer");

    // deque so a strategy's context address stays stable
    std::deque<InstanceContext> contexts;
    std::stop_source shutdown;
    strategy::Registry live{{.context_for = [&contexts, &ctx](std::uint16_t id) -> strategy::Context* {
                                 for (auto& window : contexts) {
                                     if (window.strategy_id() == id) {
                                         // reuse the window a refused create already minted
                                         return &window;
                                     }
                                 }
                                 contexts.emplace_back(ctx, id, 0x7A10ULL + id);
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
                             .wall = [&clock] { return clock.wall(); }}};
    ctx.strategy_name = [&live](std::uint16_t id) {
        const auto found = live.find(id);
        return found.has_value() ? found->name : std::string{};
    };

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
    if (joe_id != 0) {
        joe_desk = std::make_unique<joe::Desk>(joe_market, joe_sink, clock, joe_curves);
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
                [joe = joe::JoeRoutes{joe_desk.get()},
                 control = strategy::make_control(live, strategy::factory(), ctx.positions, shutdown.get_token())](
                    const http::Request& req) {
                    if (auto answer = joe(req); answer.has_value()) {
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
    while (!g_stop.load(std::memory_order_relaxed) && clock.mono() < t_end) {
        // desk intents drain first, then verdicts on every gateway, then market data
        bool order_work = false;
        OrderCmd intent{};
        for (auto& gw : gateways) {
            while (gw.intents.try_pop(intent)) {
                order_work = true;
                desk_intents += 1;
                // intents may only use the manual desk's strategy id
                if (intent.client_order_id.strategy_id() != k_manual_strategy_id) {
                    desk_refusals += 1;
                    audit_intent(intent, "not_the_desk_strategy_id");
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
                    const auto placed = ctx.submit_as(order, intent.client_order_id);
                    if (!placed.has_value()) {
                        desk_refusals += 1;
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
                                            placed.error());
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
                    journal_funding(*journal_writer, ctx.positions, charge, base_of(*instrument));
                    ++funding_charges;
                    continue;
                }
                ctx.reconcile_fee(charge, base_of(*instrument));
            }
        }

        // one path for pushed and pulled fills; by value because attribute writes our id onto it
        const auto handle_exec_event = [&](OrderEvent event, bool pulled) {
            // the pull admits before calling; a push whose execution a pull already booked is the same money
            if (!pulled && event.type == OrderEventType::Filled &&
                !ctx.seen_fills.admit(event.exec_id.view(), clock.wall())) {
                ++duplicate_pushes;
                return;
            }
            if (const auto it = ctx.inflight_.find(event.client_order_id.raw); it != ctx.inflight_.end()) {
                rt_hist.record(clock.mono().ns - it->second);
                ctx.inflight_.erase(it);
            }
            // before apply_event, which moves the leaves; see oms/attribution.hpp
            const auto who = oms::attribute(event, store);
            // a fill after the order closed: the cancel and the fill crossed on the wire
            const std::string late_key{event.exchange_client_order_id.view()};
            auto late = who.order == nullptr && event.type == OrderEventType::Filled && !late_key.empty()
                            ? ctx.closed_orders.find(late_key)
                            : ctx.closed_orders.end();
            if (late != ctx.closed_orders.end()) {
                event.client_order_id = late->second.id;
            }
            const auto stored = store.apply_event(event);
            if (!stored.has_value() && stored.error() == oms::ApplyError::BadTransition) {
                ++bad_transitions;
            }
            if (event.client_order_id.raw != 0) {
                ctx.journal_event(event);
            }
            if (stored.has_value() && (*stored)->is_terminal() && event.type != OrderEventType::Filled &&
                !who.prev_leaves.is_undef() && who.prev_leaves.raw > 0) {
                ctx.closed_orders[std::string{(*stored)->exchange_client_order_id().view()}] = {
                    .id = (*stored)->client_order_id(), .left = who.prev_leaves, .at_ns = clock.wall().ns};
            }
            if (event.type == OrderEventType::Filled) {
                ++fills;
                auto delta = oms::filled_delta(who, event);
                if (late != ctx.closed_orders.end()) {
                    const auto size = event.qty.is_undef() ? std::int64_t{0} : event.qty.raw;
                    delta = QtyLots::from_raw(std::max<std::int64_t>(0, std::min(size, late->second.left.raw)));
                    late->second.left = QtyLots::from_raw(late->second.left.raw - delta.raw);
                    ++late_fills;
                    RLOG_WARN("strategyd: late fill {} on a closed order books {} lots", late_key, delta.raw);
                } else if (who.order == nullptr) {
                    ++unattributed_fills;
                    RLOG_ERROR("strategyd: FILL {} matched no order here", event.exchange_client_order_id.view());
                }
                if (who.order != nullptr) {
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
                if (!ctx.seen_fills.admit(fill.exec_id.view(), clock.wall())) {
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
                ctx.seen_fills.prune(clock.wall(), 7ULL * 24 * 60 * 60 * 1'000'000'000ULL);
                // a pull window reaches back minutes, not hours
                std::erase_if(ctx.closed_orders,
                              [cutoff = clock.wall().ns - 15ULL * 60 * 1'000'000'000ULL](const auto& entry) {
                                  return entry.second.at_ns < cutoff;
                              });
                for (const auto& gone :
                     reconcile_open_orders(ctx, gw.account.venue, *gw.ch.open_orders, *journal_writer, clock.wall())) {
                    live.on_order_event(gone);
                }
                const auto sweep = reconcile_against_venue(
                    ctx, gw.account.venue, *gw.ch.catalogue, *gw.ch.positions, *journal_writer, clock.wall());
                // a sweep that compared nothing neither halts nor clears
                if (sweep.confirmed_break) {
                    if (gw.kernel.on(oms::KernelEvent::ReconFailed, clock.wall())) {
                        RLOG_ERROR("strategyd[{}]: venue disagreed — halted until an operator "
                                   "resumes on a book that agrees",
                                   gw.name);
                        audit_gateway(gw, "gateway.halted", R"({"why":"venue_disagreed"})");
                    }
                } else if (sweep.any_agreement) {
                    if (gw.kernel.on(oms::KernelEvent::ReconAgreed, clock.wall())) {
                        RLOG_INFO("strategyd[{}]: venue agrees — trading again", gw.name);
                        audit_gateway(gw, "gateway.running", R"({"why":"venue_agreed"})");
                    }
                }
                gw.recon_due = now_ns + k_recon_read_interval_ns;
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
               ctx.seen_fills.size());
    fmt::print("fills-unattributed={}  duplicate-pushes={}  late-fills={}\n",
               unattributed_fills,
               duplicate_pushes,
               late_fills);
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
