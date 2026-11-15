// candled: one-minute candles of every attached market, folded from the gateways' trade rings and kept in postgres, so
// a chart opens on history from the moment the stack started, and on more after a restart
//
//   candled [--dsn <postgres-dsn>] [--keep-days 30] [--backfill-hours 24]
//
// On start it asks each venue's public history for the minutes it has not stored, back to --backfill-hours ago at
// most: the stretch before a first start, and the gap a stop left
#include <raikiri/admin/registry.hpp>
#include <raikiri/candles/candle.hpp>
#include <raikiri/candles/history.hpp>
#include <raikiri/candles/store.hpp>
#include <raikiri/common/platform.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/shm/records.hpp>

#include <unistd.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace raikiri;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

constexpr std::uint64_t k_s = 1'000'000'000ULL;
// a print can arrive a little after its minute ends; the minute closes once this much later
constexpr std::uint64_t k_grace_ns = 3 * k_s;
constexpr std::uint64_t k_rescan_ns = 2 * k_s;
constexpr std::uint64_t k_flush_ns = 5 * k_s;
constexpr std::uint64_t k_report_ns = 60 * k_s;
constexpr std::uint64_t k_prune_ns = 3600 * k_s;
// a socket that reopened lost the prints of its gap: the venue's last minutes replace ours for the busiest markets
constexpr std::uint64_t k_mend_ns = 120 * k_s;
constexpr std::int32_t k_mend_minutes = 15;
constexpr std::size_t k_mend_markets = 100;
// a pass reads at most this many prints from one ring, so one busy venue cannot starve the rest
constexpr std::size_t k_pass = 8192;

// one gateway's trade ring, read from where it stood when we attached
struct Reader {
    const admin::Gateway* gateway{nullptr};
    shm::MdReader reader;
};

} // namespace

int main(int argc, char** argv) {
    const raikiri::log::Scope logging{"candled"};
    std::string dsn;
    int keep_days = 30;
    int backfill_hours = 24;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--dsn" && i + 1 < argc) {
            dsn = argv[++i];
        } else if (arg == "--keep-days" && i + 1 < argc) {
            keep_days = std::atoi(argv[++i]);
        } else if (arg == "--backfill-hours" && i + 1 < argc) {
            backfill_hours = std::atoi(argv[++i]);
        } else {
            RLOG_ERROR("usage: candled [--dsn <postgres-dsn>] [--keep-days 30] [--backfill-hours 24]");
            return 2;
        }
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("RAIKIRI_PG_DSN"); env != nullptr) {
            dsn = env;
        }
    }
    if (dsn.empty()) {
        RLOG_ERROR("candled: no database; pass --dsn or set RAIKIRI_PG_DSN");
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::optional<candles::Store> store;
    try {
        store.emplace(dsn);
    } catch (const std::exception& e) {
        RLOG_ERROR("candled: cannot open the database: {}", e.what());
        return 1;
    }

    const LiveClock clock;
    admin::Registry registry;
    const auto shm_dir = platform::runtime_dir() + "/shm";
    std::vector<Reader> readers;
    std::uint64_t epoch = ~0ULL;
    candles::MinuteBuilder builder;
    std::vector<candles::Candle> unwritten;
    std::uint64_t written = 0;
    // what the database held when we started, and the minute we started in: everything between is the venue's to fill
    const auto started = candles::minute_of(clock.wall());
    const auto window = started - backfill_hours * 60;
    std::unordered_map<std::uint64_t, candles::Store::Span> stored;
    try {
        stored = store->spans(window);
    } catch (const std::exception& e) {
        RLOG_WARN("candled: cannot read the stored minutes, backfilling the whole window: {}", e.what());
    }
    net::IoContext io;
    std::unordered_map<std::string, std::unique_ptr<net::RestClient>> rest;
    candles::Backfill backfill;
    std::unordered_set<std::string> planned; // accounts whose gaps are queued
    std::uint64_t filled = 0;
    std::unordered_map<std::string, std::int64_t> reopens; // per account, as last seen
    std::uint64_t mended = 0;

    // accounts whose sockets reopened since the last look get their recent minutes from the venue again
    const auto mend = [&] {
        std::unordered_set<std::string> hit;
        for (const auto& m : registry.venue_metrics(clock.mono(), clock.wall())) {
            auto [it, fresh] = reopens.try_emplace(m.account, m.reopens);
            if (!fresh && m.reopens > it->second) {
                hit.insert(m.account);
            }
            it->second = m.reopens;
        }
        if (hit.empty()) {
            return;
        }
        const auto now = candles::minute_of(clock.wall());
        std::unordered_map<std::string, std::size_t> taken;
        for (const auto& market : registry.markets({}, std::numeric_limits<std::size_t>::max())) {
            for (const auto& listing : market.listings) {
                if (!hit.contains(listing.account) || listing.market == "option" ||
                    taken[listing.account] >= k_mend_markets) {
                    continue;
                }
                std::uint64_t packed = 0;
                std::from_chars(
                    listing.instrument_id.data(), listing.instrument_id.data() + listing.instrument_id.size(), packed);
                backfill.add({.instrument = InstrumentId{packed},
                              .symbol = listing.exchange_symbol,
                              .from = now - k_mend_minutes,
                              .to = now});
                ++taken[listing.account];
                ++mended;
            }
        }
        RLOG_INFO(
            "candled: sockets reopened on {} gateways; their last {} minutes asked again", hit.size(), k_mend_minutes);
    };
    std::uint64_t asked = 0;

    // each attached gateway's markets, busiest first, once
    const auto plan = [&] {
        std::size_t added = 0;
        for (const auto& market : registry.markets({}, std::numeric_limits<std::size_t>::max())) {
            for (const auto& listing : market.listings) {
                if (planned.contains(listing.account) || listing.market == "option") {
                    continue;
                }
                std::uint64_t packed = 0;
                std::from_chars(
                    listing.instrument_id.data(), listing.instrument_id.data() + listing.instrument_id.size(), packed);
                // what the window lacks before the first stored minute and after the last; the started minute is ours
                // only in part, so the venue's whole one replaces it
                const InstrumentId id{packed};
                const auto have = stored.find(packed);
                if (have == stored.end()) {
                    backfill.add(
                        {.instrument = id, .symbol = listing.exchange_symbol, .from = window, .to = started + 1});
                } else {
                    backfill.add({.instrument = id,
                                  .symbol = listing.exchange_symbol,
                                  .from = window,
                                  .to = have->second.first});
                    backfill.add({.instrument = id,
                                  .symbol = listing.exchange_symbol,
                                  .from = have->second.last + 1,
                                  .to = started + 1});
                }
                ++added;
            }
        }
        for (std::size_t i = 0; i < registry.size(); ++i) {
            planned.insert(registry.at(i)->name);
        }
        if (added > 0) {
            RLOG_INFO("candled: {} markets to backfill, {} calls queued", added, backfill.queued());
        }
    };

    const auto ask = [&](const candles::Backfill::Due& due) {
        auto& client = rest[due.call.host];
        if (client == nullptr) {
            client = std::make_unique<net::RestClient>(io, net::RestConfig{.host = due.call.host, .pool_size = 2});
            client->start();
        }
        ++asked;
        const auto sent = client->send(
            {.method = due.call.body.empty() ? net::HttpMethod::Get : net::HttpMethod::Post,
             .target = due.call.target,
             .body = due.call.body},
            [&, due](net::RestResponse&& r) {
                if (r.status != 200) {
                    RLOG_WARN("candled: {}{} answered {}: {:.120}",
                              due.call.host,
                              due.call.target,
                              r.status,
                              r.transport_failed() ? r.error : r.body);
                    backfill.retry(due);
                    return;
                }
                auto got = due.source->parse(r.body, due.part.instrument);
                if (!got.has_value()) {
                    RLOG_WARN("candled: unreadable history from {}: {}", due.call.host, got.error());
                    return;
                }
                for (auto& c : *got) {
                    if (c.minute >= due.part.from && c.minute < due.part.to) {
                        unwritten.push_back(c);
                        ++filled;
                    }
                }
            });
        if (!sent) {
            backfill.defer(due);
        }
    };
    std::uint64_t overruns = 0;
    std::uint64_t unknown = 0;

    const auto flush = [&] {
        auto closed = builder.take();
        unwritten.insert(unwritten.end(), closed.begin(), closed.end());
        if (unwritten.empty()) {
            return;
        }
        try {
            store->write(unwritten);
            written += unwritten.size();
            unwritten.clear();
        } catch (const std::exception& e) {
            // kept for the next flush; a database that stays away costs memory, not candles
            RLOG_WARN("candled: write of {} minutes failed, kept for the next try: {}", unwritten.size(), e.what());
            try {
                store.emplace(dsn);
            } catch (const std::exception&) {
            }
        }
    };

    auto rescan_due = clock.mono().ns;
    auto flush_due = clock.mono().ns + k_flush_ns;
    auto report_due = clock.mono().ns + k_report_ns;
    auto prune_due = clock.mono().ns;
    auto mend_due = clock.mono().ns + k_mend_ns;
    RLOG_INFO("candled: folding prints into minutes, keeping {} days", keep_days);

    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto mono = clock.mono().ns;
        if (mono >= rescan_due) {
            rescan_due = mono + k_rescan_ns;
            (void)registry.discover(shm_dir, ::getpid());
            // a re-attach moves the mappings, so every reader starts again from its ring's head
            if (registry.epoch() != epoch) {
                epoch = registry.epoch();
                readers.clear();
                for (std::size_t i = 0; i < registry.size(); ++i) {
                    const auto* gw = registry.at(i);
                    if (gw != nullptr && gw->channels.md_events != nullptr) {
                        readers.push_back({.gateway = gw, .reader = shm::MdReader{gw->channels.md_events}});
                    }
                }
                RLOG_INFO("candled: reading the trades of {} gateways", readers.size());
            }
            plan();
        }

        std::size_t read = 0;
        for (auto& r : readers) {
            shm::MdEvent event{};
            for (std::size_t n = 0; n < k_pass; ++n) {
                const auto got = r.reader.poll(event);
                if (got == shm::PollResult::Empty) {
                    break;
                }
                if (got == shm::PollResult::Overrun) {
                    ++overruns;
                    continue;
                }
                ++read;
                if (event.kind != shm::MdKind::Trade) {
                    continue;
                }
                const auto* listed = r.gateway->channels.catalogue->find(event.instrument);
                if (listed == nullptr) {
                    ++unknown;
                    continue;
                }
                const auto& base = base_of(*listed);
                const auto ts = event.ts_event.ns != 0 ? event.ts_event : event.ts_init;
                builder.trade(
                    event.instrument, base.ticks_to_price(event.price).raw, base.lots_to_qty(event.qty).raw, ts);
            }
        }

        builder.close_through(clock.wall(), k_grace_ns);
        for (const auto& due : backfill.due(mono, candles::minute_of(clock.wall()))) {
            ask(due);
        }
        (void)io.poll();
        if (mono >= flush_due) {
            flush_due = mono + k_flush_ns;
            flush();
        }
        if (mono >= mend_due) {
            mend_due = mono + k_mend_ns;
            mend();
        }
        if (mono >= prune_due) {
            prune_due = mono + k_prune_ns;
            try {
                const auto before = candles::minute_of(clock.wall()) - keep_days * 24 * 60;
                if (const auto gone = store->prune(before); gone > 0) {
                    RLOG_INFO("candled: dropped {} minutes older than {} days", gone, keep_days);
                }
            } catch (const std::exception& e) {
                RLOG_WARN("candled: prune failed: {}", e.what());
            }
        }
        if (mono >= report_due) {
            report_due = mono + k_report_ns;
            RLOG_INFO("candled: {} prints, {} minutes written, {} open, {} late, {} overruns, {} unknown instruments; "
                      "history: {} calls, {} minutes, {} queued, {} dropped",
                      builder.prints(),
                      written,
                      builder.open_minutes(),
                      builder.late(),
                      overruns,
                      unknown,
                      asked,
                      filled,
                      backfill.queued(),
                      backfill.dropped());
        }
        if (read == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    builder.close_all();
    flush();
    RLOG_INFO("candled: down, {} minutes written", written);
    return 0;
}
