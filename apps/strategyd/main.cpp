// strategyd — the strategy kernel (demo wiring).
//
//   strategyd [--venue=<name>] [--duration <sec>] [--strategy-id=<n>]
//
// Attaches a gateway's segment, adopts the instrument numbering it published,
// runs a strategy through the OMS and prints the latency table at exit — the
// `make demo` money-shot:
//   tick-to-callback   BBO publish (gatewayd) -> strategy callback
//   submit-call        ctx.submit() wall time (risk + store + journal + ring)
//   order-round-trip   submit -> first venue verdict event back
//
// Single thread, no locks: exec ring, md ring, BBO change-gates, timers, all
// polled in one loop — the same shape the real kernel keeps forever.

#include <raikiri/journal/detail.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/observ/histogram.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/order_store.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/oms/risk.hpp>
#include <raikiri/schema/codec.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/strategies/demo_maker.hpp>
#include <raikiri/strategy/strategy.hpp>

#include <fmt/format.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

using msgs = raikiri_journal::schema::messages;
using namespace raikiri;

// Every Strategy callback defaults to a no-op, so observing a live venue
// needs no separate code path.
class Observer final : public strategy::Strategy {};

class KernelContext final : public strategy::Context {
public:
    KernelContext(shm::GatewayChannels channels,
                  oms::OrderStore& store,
                  oms::RiskGate& risk,
                  oms::IdGen& ids,
                  journal::JournalWriter& journal,
                  const Clock& clock)
        : ch_{channels}, creates_{channels.cmd_create}, cancels_{channels.cmd_cancel},
          store_{&store}, risk_{&risk}, ids_{&ids}, journal_{&journal}, clock_{&clock} {}

    std::expected<ClientOrderId, strategy::RejectReason>
    submit(const strategy::NewOrder& order) noexcept override {
        const auto t0 = clock_->mono();

        OrderCmd cmd{};
        cmd.kind = CmdKind::Create;
        cmd.side = order.side;
        cmd.order_type = order.type;
        cmd.tif = order.tif;
        cmd.flags = order.flags;
        cmd.instrument = order.instrument;
        cmd.client_order_id = ids_->next_client_id();
        cmd.exchange_client_order_id = ids_->next_exchange_client_id(clock_->wall());
        cmd.price = order.price;
        cmd.qty = order.qty;
        cmd.ts_created = clock_->wall();

        oms::Exposure exposure{.open_orders = store_->open_count()};
        if (const auto* held = position(order.instrument); held != nullptr) {
            exposure.position_lots = held->qty();
        }
        const auto working = oms::working_lots(*store_, order.instrument);
        exposure.working_buy_lots = working.buy;
        exposure.working_sell_lots = working.sell;
        if (!risk_->check(cmd, exposure).has_value()) {
            return std::unexpected{strategy::RejectReason::Risk};
        }
        if (!store_->create(cmd, cmd.exchange_client_order_id).has_value()) {
            return std::unexpected{strategy::RejectReason::Capacity};
        }
        // Intent journaled BEFORE the wire: the crash-recovery invariant.
        journal_cmd(cmd);
        if (!creates_.try_push(cmd).has_value()) {
            return std::unexpected{strategy::RejectReason::VenueDown};
        }
        // The order is on the wire: apply the local Submitted transition.
        // Without this the FSM (correctly!) rejects the venue's Accepted —
        // Initialized has no Accepted arm.
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

    bool cancel(ClientOrderId id) noexcept override {
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
        journal_cmd(cmd);
        return cancels_.try_push(cmd).has_value();
    }

    void cancel_all(InstrumentId instrument) noexcept override {
        store_->for_each_open([&](const oms::Order& order) {
            if (order.instrument() == instrument) {
                (void)cancel(order.client_order_id());
            }
        });
    }

    std::optional<Bbo> bbo(InstrumentId instrument) const noexcept override {
        const auto idx = instrument.symbol().raw;
        if (idx >= shm::k_bbo_cells) {
            return std::nullopt;
        }
        Bbo out{};
        if (ch_.bbo->read(idx, out) == 0) {
            return std::nullopt;
        }
        return out;
    }

    const Instrument* instrument(InstrumentId id) const noexcept override {
        return ch_.catalogue->find(id);
    }

    const portfolio::Position* position(InstrumentId id) const noexcept override {
        return positions.find({ids_->strategy_id(), id});
    }

    FixedE9 unrealized(InstrumentId id, PriceTicks mark) const noexcept override {
        const auto* held = position(id);
        const auto* listed = instrument(id);
        if (held == nullptr || listed == nullptr) {
            return FixedE9::from_raw(0);
        }
        return portfolio::unrealized_pnl(base_of(*listed), *held, mark);
    }

    void set_timer(std::uint32_t timer_id, std::uint64_t delay_ns) noexcept override {
        const auto deadline = clock_->mono().ns + delay_ns;
        for (auto& timer : timers) {
            if (timer.id == timer_id) {
                timer.deadline_ns = deadline; // re-arm replaces
                return;
            }
        }
        timers.push_back({timer_id, deadline});
    }

    const Clock& clock() const noexcept override { return *clock_; }

    // --- kernel-side state the main loop reads ---
    struct Timer {
        std::uint32_t id{0};
        std::uint64_t deadline_ns{0};
    };

    std::vector<Timer> timers;
    portfolio::PositionBook positions;
    std::unordered_map<std::uint64_t, std::uint64_t> inflight_; // coid -> submit mono ns
    observ::Histogram submit_hist;

private:
public:
    // The ledger is written HERE, by the process that did the arithmetic, and
    // is never re-derived downstream from the execution rows: a re-derivation
    // depends on instrument data that can change under it, so it would answer
    // a question about today with numbers from today rather than from the fill.
    void journal_pnl(const OrderEvent& event,
                     const InstrumentBase& base,
                     raikiri_journal::types::pnlKindEnum kind,
                     FixedE9 amount) noexcept {
        if (amount.raw == 0 || amount.is_undef()) {
            return; // an increment of nothing is not a fact worth keeping
        }
        (void)schema::append_msg<msgs::PnlIncrementMsg>(*journal_, event.ts_event.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.kind(kind);
            m.settleSide(portfolio::settle_side(base.kind) == portfolio::SettleSide::Base
                             ? jt::settleSideEnum::Base
                             : jt::settleSideEnum::Quote);
            m.strategyId(event.client_order_id.strategy_id());
            m.instrument(jt::instrumentId{event.instrument.packed});
            m.symbol().assign_range(base.exchange_symbol.view());
            m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
            m.execIdField().assign_range(event.exec_id.view());
            m.amount(jt::fixedE9{amount.raw});
            m.tsEvent(jt::timestampNs{event.ts_event.ns});
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

    shm::GatewayChannels ch_;
    shm::SpscProducer<OrderCmd, shm::k_cmd_ring> creates_;
    shm::SpscProducer<OrderCmd, shm::k_cmd_ring> cancels_;
    oms::OrderStore* store_;
    oms::RiskGate* risk_;
    oms::IdGen* ids_;
    journal::JournalWriter* journal_;
    const Clock* clock_;
};

} // namespace

int main(int argc, char** argv) {
    std::uint64_t duration_s = 10;
    std::string venue_name = "sim";
    // Names this strategy's book and rides in the top 16 bits of every
    // ClientOrderId, so it reaches the ledger without extra plumbing. Two
    // strategies that share it net into one position; distinct ones do not.
    std::uint16_t strategy_id = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--duration" && i + 1 < argc) {
            duration_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg.starts_with("--venue=")) {
            venue_name = arg.substr(8);
        } else if (arg.starts_with("--strategy-id=")) {
            strategy_id = static_cast<std::uint16_t>(std::atoi(arg.substr(14).data()));
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;

    // Attach the gateway segment (retry: the gateway may still be coming up).
    const auto seg_path = shm::gateway_segment_path(venue_name);
    std::expected<shm::SegmentClient, shm::SegError> client =
        std::unexpected{shm::SegError::MapFailed};
    for (int attempt = 0; attempt < 100; ++attempt) {
        client = shm::SegmentClient::attach(seg_path);
        if (client.has_value()) {
            break;
        }
        ::usleep(50'000);
    }
    if (!client.has_value()) {
        fmt::print(stderr, "strategyd: cannot attach {}\n", seg_path);
        return 1;
    }
    auto channels = shm::open_gateway_channels(*client);
    if (!channels.has_value()) {
        fmt::print(
            stderr, "strategyd: channel open failed ({})\n", static_cast<int>(channels.error()));
        return 1;
    }
    (void)client->register_reader(::getpid());

    const char* data_dir_env = std::getenv("RAIKIRI_DATA_DIR");
    const std::string data_dir = data_dir_env != nullptr ? data_dir_env : "raikiri-data";
    auto journal_writer = journal::JournalWriter::open(data_dir + "/journal/strategyd", "journal");
    if (!journal_writer.has_value()) {
        fmt::print(stderr, "strategyd: journal open failed\n");
        return 1;
    }

    oms::OrderStore store{4096};
    oms::RiskGate risk{{.max_open_orders = 64, .max_qty_lots = QtyLots::from_raw(1'000)}};
    oms::IdGen ids{strategy_id, /*seed=*/0x7A10ULL};
    KernelContext ctx{*channels, store, risk, ids, *journal_writer, clock};

    const auto instrument_count = channels->catalogue->size();
    if (instrument_count == 0) {
        fmt::print(stderr, "strategyd: gateway published an empty catalogue\n");
        return 1;
    }

    // DemoMaker quotes both sides, which is only ever appropriate against the
    // sim venue. A live gateway is observed, not traded: the loop, the
    // journal and the latency table are identical either way.
    strategies::DemoMaker maker;
    Observer observer;
    const bool quoting = venue_name == "sim";
    strategy::Strategy& active = quoting ? static_cast<strategy::Strategy&>(maker) : observer;
    fmt::print("strategyd: attached to {}, {} instruments, running {} for {}s\n",
               venue_name,
               instrument_count,
               quoting ? "DemoMaker" : "observer",
               duration_s);
    active.on_start(ctx);

    shm::BroadcastReader exec_reader{channels->exec_events};
    shm::BroadcastReader md_reader{channels->md_events};
    std::vector<std::uint32_t> bbo_seen(std::min<std::size_t>(instrument_count, shm::k_bbo_cells));

    observ::Histogram tick_hist; // BBO ts_init -> callback dispatch
    observ::Histogram rt_hist;   // submit -> first venue verdict
    std::uint64_t exec_overruns = 0;
    std::uint64_t fills = 0;
    std::uint64_t bad_transitions = 0;    // FSM refusals: must stay 0
    std::uint64_t position_refusals = 0;  // position book full: must stay 0

    const auto t_end = clock.mono() + duration_s * 1'000'000'000ULL;
    while (!g_stop.load(std::memory_order_relaxed) && clock.mono() < t_end) {
        // Verdicts drain before market data: a strategy must never act on a price
        // tick while its own order state is a poll behind.
        shm::ExecEventRec rec{};
        for (;;) {
            const auto poll = exec_reader.poll(rec);
            if (poll == shm::PollResult::Empty) {
                break;
            }
            if (poll == shm::PollResult::Overrun) {
                ++exec_overruns; // policy: halt+reconcile; demo just counts
                continue;
            }
            const auto& event = rec.event;
            if (const auto it = ctx.inflight_.find(event.client_order_id.raw);
                it != ctx.inflight_.end()) {
                rt_hist.record(clock.mono().ns - it->second);
                ctx.inflight_.erase(it);
            }
            // Read leaves BEFORE the FSM moves it: the fill quantity is the
            // drop in leaves, never the event's own qty. A venue that resends
            // a fill notification then derives zero and moves no money.
            QtyLots prev_leaves = QtyLots::from_raw(0);
            if (const auto* order = store.find(event.client_order_id); order != nullptr) {
                prev_leaves = order->leaves();
            }
            if (const auto applied = store.apply_event(event);
                !applied.has_value() && applied.error() == oms::ApplyError::BadTransition) {
                ++bad_transitions;
            }
            if (event.type == OrderEventType::Filled) {
                ++fills;
                const auto delta =
                    QtyLots::from_raw(prev_leaves.raw - event.leaves_qty.raw);
                const auto* listed = ctx.instrument(event.instrument);
                if (listed != nullptr && delta.raw > 0) {
                    const auto& base = base_of(*listed);
                    const auto applied = ctx.positions.on_fill(base, event, delta);
                    if (!applied.has_value()) {
                        ++position_refusals;
                    } else {
                        namespace jt = raikiri_journal::types;
                        // One increment per kind per exec_id: a flipped fill is
                        // still ONE realizing event to the venue, and the
                        // ledger's grain has to match the grain every
                        // reconciliation query joins on.
                        ctx.journal_pnl(event, base, jt::pnlKindEnum::Realized, applied->realized);
                        ctx.journal_pnl(event, base, jt::pnlKindEnum::Fee, event.fee);
                    }
                }
            }
            active.on_order_event(ctx, event);
        }
        shm::MdEvent md{};
        while (md_reader.poll(md) == shm::PollResult::Item) {
            TradeTick trade{};
            trade.instrument = md.instrument;
            trade.price = md.price;
            trade.qty = md.qty;
            trade.aggressor = md.aggressor;
            trade.ts_event = md.ts_event;
            trade.ts_init = md.ts_init;
            active.on_trade(ctx, trade);
        }
        for (std::uint32_t sym = 0; sym < bbo_seen.size(); ++sym) {
            const auto version = channels->bbo->cells[sym].version_now();
            if (version == bbo_seen[sym] || (version & 1U) != 0) {
                continue;
            }
            Bbo bbo{};
            if (channels->bbo->read(sym, bbo) != 0) {
                bbo_seen[sym] = version;
                tick_hist.record(clock.wall().ns - bbo.ts_init.ns);
                active.on_bbo(ctx, bbo);
            }
        }
        const auto now_ns = clock.mono().ns;
        for (auto& timer : ctx.timers) {
            if (timer.deadline_ns != 0 && now_ns >= timer.deadline_ns) {
                timer.deadline_ns = 0;
                active.on_timer(ctx, timer.id);
            }
        }
    }
    active.on_stop(ctx);
    journal_writer->flush();

    const auto tick = tick_hist.snapshot();
    const auto submit = ctx.submit_hist.snapshot();
    const auto rt = rt_hist.snapshot();
    fmt::print(
        "\n===== raikiri: {} venue, {}s, single unpinned core =====\n", venue_name, duration_s);
    fmt::print("{:<22}{:>10}{:>12}{:>12}{:>12}{:>12}\n",
               "metric (ns)",
               "count",
               "p50",
               "p90",
               "p99",
               "max");
    const auto row = [](const char* name, const observ::HistogramSnapshot& s) {
        fmt::print("{:<22}{:>10}{:>12}{:>12}{:>12}{:>12}\n",
                   name,
                   s.count,
                   s.p50_ns,
                   s.p90_ns,
                   s.p99_ns,
                   s.max_ns);
    };
    row("tick-to-callback", tick);
    row("submit-call", submit);
    row("order-round-trip", rt);
    fmt::print("fills={}  open-at-exit={}  exec-ring-overruns={}  fsm-refusals={}  "
               "position-refusals={}\n",
               fills,
               store.open_count(),
               exec_overruns,
               bad_transitions,
               position_refusals);

    if (ctx.positions.size() > 0) {
        fmt::print("\n{:<16}{:>12}{:>16}{:>16}{:>16}\n",
                   "instrument",
                   "net (lots)",
                   "avg entry",
                   "realized",
                   "unrealized");
        char buf[32]{};
        for (const auto& position : ctx.positions.all()) {
            const auto* listed = ctx.instrument(position.key().instrument);
            if (listed == nullptr) {
                continue;
            }
            const auto& base = base_of(*listed);
            // Marked at the last BBO mid; an unrealized number is only ever as
            // good as the price the caller marks it against.
            auto mark = PriceTicks::from_raw(0);
            if (const auto bbo = ctx.bbo(position.key().instrument); bbo.has_value()) {
                mark = PriceTicks::from_raw((bbo->bid.raw + bbo->ask.raw) / 2);
            }
            const auto fixed = [&buf](FixedE9 v) -> std::string {
                return v.is_undef() ? std::string{"-"}
                                    : std::string{format_e9(v, buf, sizeof(buf))};
            };
            fmt::print("{:<16}{:>12}{:>16}{:>16}{:>16}\n",
                       base.exchange_symbol.view(),
                       position.qty().raw,
                       fixed(position.avg_entry_price(base)),
                       fixed(position.net_realized()),
                       mark.raw == 0 ? std::string{"-"}
                                     : fixed(portfolio::unrealized_pnl(base, position, mark)));
        }
    }
    return 0;
}
