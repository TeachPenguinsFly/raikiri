// what the audit trail costs an order, how much it writes, and how a query holds up against a busy day

#include <raikiri/audit/file_sink.hpp>
#include <raikiri/audit/file_store.hpp>
#include <raikiri/audit/file_writer.hpp>
#include <raikiri/audit/query.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/oms/order_trail.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace {

using namespace raikiri;
using Steady = std::chrono::steady_clock;

class Silent final : public audit::Sink {
public:
    void append(const audit::Entry& entry) noexcept override { bytes += entry.detail_json.size(); }

    std::size_t bytes{0};
};

OrderEvent fill_event(std::uint64_t seq, UnixNanos now) {
    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
    out.client_order_id = ClientOrderId::make(7, seq);
    out.exchange_client_order_id = *ExchangeClientOrderId::from("01JABCDEFGHJKMNPQRSTVWXYZ0");
    out.exchange_order_id = *ExchangeOrderId::from("8389765012345678");
    out.exec_id = *ExecId::from("exec-1234567890");
    out.price = PriceTicks::from_raw(6'500'012);
    out.qty = QtyLots::from_raw(3);
    out.leaves_qty = QtyLots::from_raw(7);
    out.fee = FixedE9::from_raw(1'500'000);
    out.fee_asset = to_fee_asset("USDT");
    out.ts_event = now;
    out.ts_init = now;
    return out;
}

double millis_since(Steady::time_point start) {
    return std::chrono::duration<double, std::milli>(Steady::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    const raikiri::log::Scope logging{"order_trail"};
    const auto lines = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : std::size_t{2'000'000};
    const auto root = std::filesystem::temp_directory_path() / "order_trail_bench";
    std::filesystem::remove_all(root);
    const LiveClock clock;
    const auto now = clock.wall();
    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
    cmd.client_order_id = ClientOrderId::make(7, 1);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01JABCDEFGHJKMNPQRSTVWXYZ0");
    cmd.price = PriceTicks::from_raw(6'500'012);
    cmd.qty = QtyLots::from_raw(10);
    const oms::Order order{cmd, cmd.exchange_client_order_id};

    constexpr std::size_t k_rounds = 2'000'000;
    {
        Silent sink;
        oms::OrderTrail trail{sink};
        const auto event = fill_event(1, now);
        const auto start = Steady::now();
        for (std::size_t i = 0; i < k_rounds; ++i) {
            trail.event(event, &order, now);
        }
        const auto ms = millis_since(start);
        fmt::print("format only:       {:7.0f} ns/event  {} bytes of detail\n",
                   ms * 1e6 / static_cast<double>(k_rounds),
                   sink.bytes / k_rounds);
    }
    {
        auto sink = audit::FileSink::open((root / "queue").string(), {.fsync = audit::FsyncPolicy::Never}, clock);
        oms::OrderTrail trail{**sink};
        const auto event = fill_event(1, now);
        constexpr std::size_t k_burst = 2000;
        constexpr int k_bursts = 200;
        std::vector<double> samples;
        for (int b = 0; b < k_bursts; ++b) {
            const auto start = Steady::now();
            for (std::size_t i = 0; i < k_burst; ++i) {
                trail.event(event, &order, now);
            }
            samples.push_back(millis_since(start) * 1e6 / static_cast<double>(k_burst));
            (*sink)->wait_drained();
        }
        std::ranges::sort(samples);
        const auto stats = (*sink)->stats();
        fmt::print("format and queue:  {:7.0f} ns/event median, {:.0f} worst burst, dropped {}\n",
                   samples[samples.size() / 2],
                   samples.back(),
                   stats.dropped);
    }
    for (const auto policy : {audit::FsyncPolicy::Never, audit::FsyncPolicy::EachBatch}) {
        const auto dir = root / (policy == audit::FsyncPolicy::Never ? "drain_never" : "drain_batch");
        auto sink = audit::FileSink::open(dir.string(), {.fsync = policy}, clock);
        oms::OrderTrail trail{**sink};
        const auto event = fill_event(1, now);
        constexpr std::size_t k_lines = 200'000;
        const auto start = Steady::now();
        for (std::size_t i = 0; i < k_lines; ++i) {
            trail.event(event, &order, now);
            if (i % 2048 == 0) {
                (*sink)->wait_drained();
            }
        }
        (*sink)->wait_drained();
        const auto ms = millis_since(start);
        fmt::print("writer {:>6}:     {:7.0f} lines/s through the chain, dropped {}\n",
                   policy == audit::FsyncPolicy::Never ? "never" : "batch",
                   static_cast<double>(k_lines) / ms * 1e3,
                   (*sink)->stats().dropped);
    }

    // a busy day's file
    const auto day_dir = root / "days" / "strategyd";
    std::filesystem::create_directories(day_dir);
    std::uint64_t file_bytes = 0;
    {
        auto writer = *audit::FileWriter::open(day_dir.string(), audit::FsyncPolicy::Never, clock);
        std::array<char, 40> subject{};
        const std::string detail(330, 'x');
        const auto wrapped = fmt::format(R"({{"pad":"{}"}})", detail);
        constexpr std::array<std::string_view, 4> k_actions{
            "order.created", "order.sent", "order.acked", "order.filled"};
        const auto start = Steady::now();
        const std::uint64_t midnight = (now.ns / (86'400ULL * 1'000'000'000ULL)) * (86'400ULL * 1'000'000'000ULL);
        for (std::size_t i = 0; i < lines; ++i) {
            const auto wrote = fmt::format_to_n(subject.data(), subject.size(), "order:{}", i / 4);
            (void)writer.append({.ts = UnixNanos{midnight + (i * 10'000)},
                                 .actor = "strategy:7",
                                 .action = k_actions[i % 4],
                                 .subject = std::string_view{subject.data(), wrote.size},
                                 .detail_json = wrapped});
        }
        (void)writer.flush();
        fmt::print("generated {} lines in {:.0f} ms\n", lines, millis_since(start));
    }
    for (const auto& item : std::filesystem::directory_iterator{day_dir}) {
        if (item.path().extension() == ".jsonl") {
            file_bytes = item.file_size();
            fmt::print("day file:  {} MB, {} bytes per line\n", file_bytes >> 20, file_bytes / lines);
        } else if (item.path().extension() == ".idx") {
            fmt::print("day index: {} MB\n", item.file_size() >> 20);
        }
    }
    const audit::FileStore store{(root / "days").string()};
    const audit::QueryAudit query{store};
    const auto days = store.days("strategyd");
    {
        const auto start = Steady::now();
        std::uint64_t count = 0;
        store.scan_day("strategyd", days.back(), [&](const audit::EntryView& entry) {
            count += entry.subject.size() == 13 ? 1U : 0U;
        });
        fmt::print("scan every line (the old way): {:8.1f} ms\n", millis_since(start));
    }
    const auto timed = [&](std::string_view name, const audit::AuditQuery& q) {
        const auto start = Steady::now();
        const auto page = query(q);
        fmt::print("{:<36}{:8.1f} ms, {} rows\n", name, millis_since(start), page.items.size());
        return page;
    };
    const auto newest = timed("newest page", {.limit = 200});
    (void)timed("next page", {.limit = 200, .after = newest.next});
    (void)timed("one order, exact subject", {.subject = fmt::format("order:{}", lines / 8), .limit = 200});
    (void)timed("one action, exact", {.action = "order.filled", .limit = 200});
    (void)timed("a prefix, newest page", {.subject = "order:*", .limit = 200});
    (void)timed("an order that does not exist", {.subject = "order:nope", .limit = 200});
    {
        const auto start = Steady::now();
        (void)store.verify("strategyd", days.back());
        fmt::print("{:<36}{:8.1f} ms\n", "verify, first walk", millis_since(start));
        const auto again = Steady::now();
        (void)store.verify("strategyd", days.back());
        fmt::print("{:<36}{:8.1f} ms\n", "verify, nothing new", millis_since(again));
    }
    std::filesystem::remove_all(root);
    return 0;
}
