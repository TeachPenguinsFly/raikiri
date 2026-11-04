// the order path strategyd runs on its loop, built from the same pieces in the same order: submit up to the gateway
// ring, an order's events, a joe tick and a book-risk sample. Each op is timed alone, allocations counted on this
// thread, so the cleanup between ops costs nothing

#include <raikiri/audit/file_sink.hpp>
#include <raikiri/bookrisk/desk.hpp>
#include <raikiri/bookrisk/value.hpp>
#include <raikiri/common/sanitizer.hpp>
#include <raikiri/joe/desk.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/observ/histogram.hpp>
#include <raikiri/oms/exec_path.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/order_store.hpp>
#include <raikiri/oms/order_trail.hpp>
#include <raikiri/oms/repeat_guard.hpp>
#include <raikiri/oms/replay.hpp>
#include <raikiri/oms/risk.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/shm/records.hpp>

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <unordered_map>
#include <vector>

namespace {

thread_local std::uint64_t t_allocs = 0;

} // namespace

#if !RAIKIRI_TSAN
// gcc pairs this malloc with the libc free below and reads a mismatch once they inline together
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n); p != nullptr) {
        return p;
    }
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
#endif

namespace {

using namespace raikiri;
using Steady = std::chrono::steady_clock;

constexpr std::uint16_t k_book = 7;
constexpr std::uint16_t k_books = 4;
constexpr std::int64_t k_instruments = 16;
constexpr std::uint64_t k_s = 1'000'000'000ULL;

FixedE9 d(std::string_view text) {
    return *parse_decimal_e9(text);
}

InstrumentId instrument_at(std::int64_t i) {
    return InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{static_cast<std::uint32_t>(i + 1)});
}

Instrument perp_at(std::int64_t i) {
    InstrumentBase base{};
    base.id = instrument_at(i);
    base.tick_size = d("0.01");
    base.lot_size = d("0.001");
    base.contract_value = d("1");
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, 28'800};
}

class Kernel final : public bookrisk::BookKernel {
public:
    explicit Kernel(const portfolio::PositionBook& positions, const std::vector<Instrument>& listed)
        : positions_{&positions}, listed_{&listed} {}

    [[nodiscard]] oms::RiskConfig base_config(bookrisk::BookId) const override { return base_defaults(); }

    [[nodiscard]] oms::RiskConfig base_defaults() const override {
        oms::RiskConfig config{};
        config.max_open_orders = 1'000'000;
        config.max_strategy_notional = d("1000000000");
        return config;
    }

    [[nodiscard]] std::string name_of(bookrisk::BookId book) const override { return "book-" + std::to_string(book); }

    void known_books(std::vector<bookrisk::BookId>& out) const override {
        out.insert(out.end(), books.begin(), books.end());
    }

    std::size_t cancel_open(std::optional<bookrisk::BookId>) override { return 0; }

    [[nodiscard]] std::size_t open_orders(std::optional<bookrisk::BookId>) const override { return 0; }

    [[nodiscard]] bookrisk::PnlReading pnl(bookrisk::BookId book) const override {
        bookrisk::PnlReading out{};
        for (const auto& position : positions_->all()) {
            if (position.key().strategy_id == book) {
                const auto index = static_cast<std::size_t>(position.key().instrument.symbol().raw - 1);
                bookrisk::add_position(out,
                                       (*listed_)[index % listed_->size()],
                                       position,
                                       bookrisk::Mark{.mid = PriceTicks::from_raw(6'500'000)});
            }
        }
        return out;
    }

    [[nodiscard]] bookrisk::BalanceReading balances() const override { return {}; }

    std::vector<bookrisk::BookId> books{k_book};

private:
    const portfolio::PositionBook* positions_;
    const std::vector<Instrument>* listed_;
};

class NoStore final : public bookrisk::SettingsStore {
public:
    [[nodiscard]] std::expected<std::optional<bookrisk::DeskSettings>, std::string> load() override {
        return std::nullopt;
    }

    [[nodiscard]] std::expected<void, std::string> save(const bookrisk::DeskSettings&) override { return {}; }
};

class NoAudit final : public audit::Sink {
public:
    void append(const audit::Entry&) noexcept override {}
};

class NoNotices final : public bookrisk::Notifier {
public:
    void notify(const bookrisk::Notice&) override {}
};

struct Held {
    QtyLots position{QtyLots::from_raw(0)};
    QtyLots buy{QtyLots::from_raw(0)};
    QtyLots sell{QtyLots::from_raw(0)};
};

// what the strategy loop holds, with the store already carrying `open` orders over several books
struct Path {
    Path(std::int64_t open, std::int64_t position_count)
        : root{std::filesystem::temp_directory_path() / "trading_path_bench"}, journal{[&] {
              std::filesystem::remove_all(root);
              return std::move(*journal::JournalWriter::open(root.string() + "/journal", "journal"));
          }()},
          sink{std::move(*audit::FileSink::open(
              root.string() + "/audit", audit::SinkOptions{.fsync = audit::FsyncPolicy::Never}, clock))},
          trail{*sink}, ring{std::make_unique<shm::CmdRing>()}, creates{ring.get()}, consumer{ring.get()},
          positions{static_cast<std::size_t>(position_count + 8)}, desk{kernel, settings, quiet_audit, notices, clock} {
        for (std::int64_t i = 0; i < k_instruments; ++i) {
            listed.push_back(perp_at(i));
        }
        (void)desk.load();
        (void)desk.set_capital(d("1000000000"), {.by = "bench", .reason = "bench", .confirm = true});
        const auto now = clock.wall();
        for (std::int64_t i = 0; i < position_count; ++i) {
            OrderEvent fill{};
            fill.type = OrderEventType::Filled;
            fill.side = Side::Buy;
            fill.instrument = instrument_at(i % k_instruments);
            fill.client_order_id = ClientOrderId::make(static_cast<std::uint16_t>(k_book + i / k_instruments), 1);
            fill.price = PriceTicks::from_raw(6'500'000);
            fill.fee = FixedE9::from_raw(0);
            fill.ts_event = now;
            (void)positions.on_fill(listed[static_cast<std::size_t>(i % k_instruments)], fill, QtyLots::from_raw(10));
        }
        std::uint64_t seq = 1'000'000;
        for (std::int64_t i = 0; i < open; ++i) {
            OrderCmd cmd{};
            cmd.kind = CmdKind::Create;
            cmd.side = i % 2 == 0 ? Side::Buy : Side::Sell;
            cmd.order_type = OrderType::Limit;
            cmd.tif = TimeInForce::Gtc;
            cmd.instrument = instrument_at(i % k_instruments);
            cmd.client_order_id = ClientOrderId::make(static_cast<std::uint16_t>(k_book + i % k_books), ++seq);
            cmd.exchange_client_order_id = ids.next_exchange_client_id(now);
            cmd.price = PriceTicks::from_raw(6'500'000);
            cmd.qty = QtyLots::from_raw(10);
            cmd.ts_created = now;
            (void)store.create(cmd, cmd.exchange_client_order_id);
        }
    }

    ~Path() {
        sink.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    [[nodiscard]] std::unordered_map<std::uint64_t, Held> held_by(std::uint16_t book) const {
        std::unordered_map<std::uint64_t, Held> out;
        for (const auto& position : positions.all()) {
            if (position.key().strategy_id == book && position.qty().raw != 0) {
                out[position.key().instrument.packed].position = position.qty();
            }
        }
        store.for_each_open_of(book, [&](const oms::Order& open) {
            auto& held = out[open.instrument().packed];
            auto& side = open.side() == Side::Buy ? held.buy : held.sell;
            side = side + open.leaves();
        });
        return out;
    }

    [[nodiscard]] FixedE9 open_notional_besides(const std::unordered_map<std::uint64_t, Held>& held,
                                                InstrumentId skip) const {
        Notional128 total{};
        for (const auto& [packed, lots] : held) {
            const auto open = oms::open_lots(lots.position, lots.buy, lots.sell);
            if (packed == skip.packed || open == 0) {
                continue;
            }
            const auto index = static_cast<std::size_t>(InstrumentId{packed}.symbol().raw - 1);
            const auto value = oms::notional(
                listed[index % listed.size()], PriceTicks::from_raw(6'500'000), QtyLots::from_raw(open), {});
            if (!value.has_value()) {
                return FixedE9::undef();
            }
            total = total + *value;
        }
        return oms::to_e9(total);
    }

    ClientOrderId submit(std::int64_t i) noexcept {
        const auto t0 = clock.mono();
        const auto id = ids.next_client_id();
        OrderCmd cmd{};
        cmd.kind = CmdKind::Create;
        cmd.side = Side::Buy;
        cmd.order_type = OrderType::Limit;
        cmd.tif = TimeInForce::Gtc;
        cmd.instrument = instrument_at(i % k_instruments);
        cmd.client_order_id = id;
        cmd.exchange_client_order_id = ids.next_exchange_client_id(clock.wall());
        cmd.price = PriceTicks::from_raw(6'500'000 + i % 1000);
        cmd.qty = QtyLots::from_raw(10);
        cmd.ts_created = clock.wall();
        const auto& gate = desk.gate(k_book);
        const oms::OrderShape shape{.book = k_book,
                                    .instrument = cmd.instrument,
                                    .side = cmd.side,
                                    .price = cmd.price,
                                    .qty = cmd.qty,
                                    .repeat_key = static_cast<std::uint64_t>(i)};
        if (repeats.held(shape, cmd.ts_created, gate.config().duplicate_window_ns).has_value()) {
            return {};
        }
        const auto held = held_by(k_book);
        oms::Exposure exposure{.open_orders = store.open_count_of(k_book),
                               .instrument = &listed[static_cast<std::size_t>(i % k_instruments)]};
        if (const auto it = held.find(cmd.instrument.packed); it != held.end()) {
            exposure.position_lots = it->second.position;
            exposure.working_buy_lots = it->second.buy;
            exposure.working_sell_lots = it->second.sell;
        }
        exposure.reference_price = PriceTicks::from_raw(6'500'000);
        exposure.bid = exposure.reference_price;
        exposure.ask = PriceTicks::from_raw(6'500'010);
        exposure.quote_ts = cmd.ts_created;
        exposure.strategy_open_notional = open_notional_besides(held, cmd.instrument);
        desk.admit(k_book, cmd, exposure);
        if (const auto allowed = gate.check(cmd, exposure); !allowed.has_value()) {
            ++refused;
            last_refusal = oms::to_string(allowed.error().why);
            return {};
        }
        if (!store.create(cmd, cmd.exchange_client_order_id).has_value()) {
            ++refused;
            return {};
        }
        trail.command("order.created", cmd, cmd.ts_created);
        oms::append_order_cmd(journal, cmd);
        if (!creates.try_push(cmd).has_value()) {
            ++refused;
            return {};
        }
        desk.sent(k_book, cmd, exposure);
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
        (void)store.apply_event(submitted);
        trail.command("order.sent", cmd, cmd.ts_created);
        repeats.sent(shape, id, cmd.ts_created);
        inflight[id.raw] = t0.ns;
        hist.record(clock.mono().ns - t0.ns);
        return id;
    }

    // what the loop does with one pushed event, ledger rows aside
    void on_event(OrderEvent event) noexcept {
        const auto wall = clock.wall();
        const auto done = exec.apply(event, false, wall);
        if (done.repeat) {
            return;
        }
        if (const auto it = inflight.find(event.client_order_id.raw); it != inflight.end()) {
            rt.record(clock.mono().ns - it->second);
            inflight.erase(it);
        }
        oms::append_order_event(journal, event);
        trail.event(event, done.order, wall);
        if (done.order != nullptr && done.order->is_terminal() && done.order->filled().raw == 0) {
            repeats.withdrawn(done.order->client_order_id());
        }
        if (event.type == OrderEventType::Filled && done.delta.raw > 0) {
            const auto index = static_cast<std::size_t>(event.instrument.symbol().raw - 1);
            desk.filled(event.client_order_id.strategy_id(), event.instrument);
            (void)positions.on_fill(listed[index % listed.size()], event, done.delta);
        }
    }

    [[nodiscard]] OrderEvent
    event_of(OrderEventType type, ClientOrderId id, std::int64_t i, std::int64_t qty, std::int64_t leaves) const {
        OrderEvent out{};
        out.type = type;
        out.side = Side::Buy;
        out.instrument = instrument_at(i % k_instruments);
        out.client_order_id = id;
        if (const auto* order = store.find(id); order != nullptr) {
            out.exchange_client_order_id = order->exchange_client_order_id();
        }
        out.exchange_order_id = *ExchangeOrderId::from("8389765012345678");
        out.exec_id = *ExecId::from(("x" + std::to_string(i)).c_str());
        out.price = PriceTicks::from_raw(6'500'000);
        out.qty = QtyLots::from_raw(qty);
        out.leaves_qty = QtyLots::from_raw(leaves);
        out.fee = FixedE9::from_raw(1'500'000);
        out.fee_asset = to_fee_asset("USDT");
        out.ts_event = clock.wall();
        out.ts_init = out.ts_event;
        return out;
    }

    void settle() {
        OrderCmd out{};
        while (consumer.try_pop(out)) {
        }
        sink->wait_drained();
    }

    std::filesystem::path root;
    TestClock clock{UnixNanos{1'800'000'000 * k_s}};
    journal::JournalWriter journal;
    std::unique_ptr<audit::FileSink> sink;
    oms::OrderTrail trail;
    std::unique_ptr<shm::CmdRing> ring;
    shm::CmdProducer creates;
    shm::CmdConsumer consumer;
    oms::OrderStore store{1U << 17U};
    oms::ExecPath exec{store};
    oms::RepeatGuard repeats;
    oms::IdGen ids{k_book, 1};
    std::vector<Instrument> listed;
    portfolio::PositionBook positions;
    Kernel kernel{positions, listed};
    NoStore settings;
    NoAudit quiet_audit;
    NoNotices notices;
    bookrisk::RiskDesk desk;
    std::unordered_map<std::uint64_t, std::uint64_t> inflight;
    observ::Histogram hist;
    observ::Histogram rt;
    std::uint64_t refused{0};
    std::string_view last_refusal{};
};

template <typename Op, typename Between> void timed(benchmark::State& state, Op&& op, Between&& between) {
    std::uint64_t allocs = 0;
    std::uint64_t ops = 0;
    for (auto _ : state) {
        const auto before = t_allocs;
        const auto start = Steady::now();
        op();
        const auto spent = Steady::now() - start;
        allocs += t_allocs - before;
        ++ops;
        state.SetIterationTime(std::chrono::duration<double>(spent).count());
        between();
    }
    state.counters["allocs"] = benchmark::Counter(static_cast<double>(allocs) / static_cast<double>(ops));
}

void bm_submit(benchmark::State& state) {
    const raikiri::log::Scope logging{"trading_path"};
    Path path{state.range(0), state.range(1)};
    const auto ns_per_order = static_cast<std::uint64_t>(state.range(2));
    std::int64_t i = 0;
    ClientOrderId last{};
    timed(
        state,
        [&] { last = path.submit(++i); },
        [&] {
            auto closed = path.event_of(OrderEventType::Canceled, last, i, 0, 0);
            (void)path.store.apply_event(closed);
            path.inflight.erase(last.raw);
            path.clock.advance(ns_per_order);
            if (i % 1024 == 0) {
                path.settle();
            }
        });
    state.counters["refused"] = static_cast<double>(path.refused);
    if (path.refused != 0) {
        state.SetLabel(std::string{path.last_refusal});
    }
    state.counters["dropped"] = static_cast<double>(path.sink->stats().dropped);
}

void bm_order_event(benchmark::State& state) {
    const raikiri::log::Scope logging{"trading_path"};
    Path path{state.range(0), 16};
    std::int64_t i = 0;
    ClientOrderId last = path.submit(++i);
    int phase = 0;
    OrderEvent next = path.event_of(OrderEventType::Accepted, last, i, 0, 10);
    timed(
        state,
        [&] { path.on_event(next); },
        [&] {
            path.clock.advance(1'000'000);
            phase = (phase + 1) % 3;
            if (phase == 1) {
                next = path.event_of(OrderEventType::Filled, last, i, 4, 6);
            } else if (phase == 2) {
                next = path.event_of(OrderEventType::Filled, last, i + 1'000'000'000, 6, 0);
            } else {
                last = path.submit(++i);
                next = path.event_of(OrderEventType::Accepted, last, i, 0, 10);
            }
            if (i % 1024 == 0) {
                path.settle();
            }
        });
    state.counters["dropped"] = static_cast<double>(path.sink->stats().dropped);
}

void bm_book_risk_tick(benchmark::State& state) {
    const raikiri::log::Scope logging{"trading_path"};
    TestClock clock{UnixNanos{1'800'000'000 * k_s}};
    Path path{0, state.range(0) * k_instruments};
    Kernel kernel{path.positions, path.listed};
    kernel.books.clear();
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        kernel.books.push_back(static_cast<bookrisk::BookId>(k_book + i));
    }
    NoStore settings;
    NoAudit quiet;
    NoNotices notices;
    bookrisk::RiskDesk desk{kernel, settings, quiet, notices, clock};
    (void)desk.load();
    timed(state, [&] { desk.tick(); }, [&] { clock.advance(k_s); });
}

class Market final : public joe::MarketView {
public:
    [[nodiscard]] std::vector<joe::VenueBook> books(const joe::Listing&, Side side) const override {
        return {{.instrument = instrument_at(0),
                 .lot = d("0.001"),
                 .levels = {{side == Side::Buy ? d("100") : d("99.98"), d("1")}},
                 .amend = true}};
    }

    [[nodiscard]] std::optional<joe::Scale> scale(InstrumentId) const override {
        return joe::Scale{.listing = {.symbol = *SymbolText::from("BTC/USDT")}, .tick = d("0.01"), .lot = d("0.001")};
    }
};

class Sink final : public joe::OrderSink {
public:
    [[nodiscard]] bool running() const noexcept override { return true; }

    [[nodiscard]] std::expected<std::uint64_t, joe::SinkError> submit(const joe::ChildOrder&) noexcept override {
        return ++next;
    }

    void cancel(std::uint64_t) noexcept override {}

    std::uint64_t next{0};
};

void bm_joe_tick(benchmark::State& state) {
    const raikiri::log::Scope logging{"trading_path"};
    Market market;
    Sink sink;
    TestClock clock{UnixNanos{1'800'000'000 * k_s}};
    joe::CurveProfile curves;
    joe::Desk desk{market, sink, clock, curves};
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        joe::ParentSpec spec{};
        spec.listing.symbol = *SymbolText::from("BTC/USDT");
        spec.qty = d("5");
        spec.algo = joe::Algo::Twap;
        spec.horizon_ns = 24 * 3600 * k_s;
        spec.slices = 1000;
        spec.max_child_notional = d("100000");
        if (!desk.start(spec).has_value()) {
            state.SkipWithError("no parent");
            return;
        }
    }
    desk.tick();
    timed(state, [&] { desk.tick(); }, [&] { clock.advance(250'000'000); });
    state.counters["children"] = static_cast<double>(sink.next);
}

} // namespace

BENCHMARK(bm_submit)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond)
    ->ArgNames({"open", "positions", "ns_apart"})
    ->Args({0, 8, 10'000'000})
    ->Args({64, 8, 10'000'000})
    ->Args({1024, 64, 10'000'000})
    ->Args({16384, 64, 10'000'000})
    ->Args({1024, 512, 10'000'000})
    ->Args({64, 8, 100'000});
BENCHMARK(bm_order_event)->UseManualTime()->Unit(benchmark::kNanosecond)->Arg(64)->Arg(16384);
BENCHMARK(bm_book_risk_tick)->UseManualTime()->Unit(benchmark::kNanosecond)->Arg(1)->Arg(16)->Arg(64);
BENCHMARK(bm_joe_tick)->UseManualTime()->Unit(benchmark::kMicrosecond)->Arg(16)->Arg(256)->Arg(1024)->Iterations(2000);
BENCHMARK_MAIN();
