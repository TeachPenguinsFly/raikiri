// kowalski — the arbitrage scanner.
//
//   kowalski [--port 8790] [--shm <dir>] [--min-volume <quote units>]
//            [--duration <sec>]
//
// A THIRD READER OF THE GATEWAY SEGMENTS, beside strategyd and admind. It asks
// no venue for anything: the quote, the funding rate, the interval and the
// 24-hour turnover are already in shm because a gateway subscribed its whole
// catalogue and is writing them continuously. Reading them costs a seqlock load
// and no network at all, which is why this process is cheap enough to run
// beside everything else.
//
// `shm::k_max_readers` is 4. strategyd and admind hold two; this makes three.
// That ceiling is the reason the strategy service grew inside strategyd rather
// than becoming a fourth process.
//
// STATE IS IN MEMORY AND NOWHERE ELSE. A board is derived from data that is
// itself derived, and cheap to rebuild; persisting it would create a second
// source of truth that is wrong the moment a gateway restarts.
//
// A DEAD GATEWAY CONTRIBUTES NOTHING. Its last prices are still in the segment
// and still readable, and pricing a route off them would be a board describing
// a market nobody is watching — the same defect the smoke scripts had until
// they learned to check `alive` first.

#include <raikiri/common/platform.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/scan/board.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <glaze/glaze.hpp>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// The wire shape, in a NAMED namespace on purpose: glaze reflects member names
// through a variable template, and a type with no linkage cannot be reflected.
namespace raikiri::kowalski {

// What one row looks like on the wire. Every number that cannot survive an
// IEEE double travels as a string, the same rule the admin contract states:
// a rate at 1e-9 and a 24-hour turnover in the billions do not both fit.
struct WireLeg {
    std::string account;
    std::string venue;
    std::string symbol; // the venue's own spelling
    std::string bid;
    std::string ask;
    std::string rate;        // as stated, for this venue's own interval
    std::string rate_per_8h; // the same rate on the common clock
    std::uint32_t interval_s{0};
    std::string next_funding_time;
    std::string volume_24h_quote;
};

struct WireRow {
    std::string symbol; // canonical, venue-neutral: "BTC/USDT"
    WireLeg long_leg;
    WireLeg short_leg;
    std::string funding_edge_8h;
    std::int64_t price_spread_bp{0};
    std::string volume_floor;
};

struct WireBoard {
    std::string ts;
    std::size_t venues{0};
    std::size_t symbols{0};
    std::size_t considered{0}; // pairs priced, before the cut
    std::vector<WireRow> rows;
};

} // namespace raikiri::kowalski

namespace {

using namespace raikiri;
using namespace raikiri::kowalski;

std::atomic<bool> g_stop{false};

void on_signal(int) noexcept {
    g_stop.store(true, std::memory_order_relaxed);
}

// One attached gateway. Read-only: this process opens no command ring and can
// place no order, which is a property worth keeping structural rather than
// remembering.
struct Source {
    AccountId account{};
    std::string name;
    shm::SegmentClient client;
    shm::GatewayChannels ch{};

    Source(AccountId id, std::string display, shm::SegmentClient attached, shm::GatewayChannels c)
        : account{id}, name{std::move(display)}, client{std::move(attached)}, ch{c} {}

    Source(Source&&) = default;
    Source& operator=(Source&&) = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    ~Source() = default;
};

[[nodiscard]] std::string digits(std::uint64_t value) {
    return std::to_string(value);
}

[[nodiscard]] std::string decimal(FixedE9 value) {
    if (value.is_undef()) {
        return {};
    }
    std::array<char, 48> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

// Everything one venue says about one instrument, gathered in one pass so the
// three tables are read at the same instant rather than a tick apart.
struct Gathered {
    scan::Leg leg;
    std::string account;
    std::string exchange_symbol;
    FixedE9 rate_per_hour{};
};

// Builds the board. Pure of I/O apart from the seqlock reads, and every
// exclusion below is a decision the board would otherwise make silently.
[[nodiscard]] WireBoard build(const std::vector<std::unique_ptr<Source>>& sources,
                              FixedE9 min_volume,
                              std::size_t limit,
                              UnixNanos now,
                              MonoNanos mono) {
    std::map<std::string, std::vector<Gathered>> by_symbol;
    std::size_t live_venues = 0;

    for (const auto& owned : sources) {
        const auto& src = *owned;
        if (!src.client.owner_alive(mono.ns)) {
            continue; // its prices are last-known, not current
        }
        ++live_venues;
        for (const auto& listed : src.ch.catalogue->all()) {
            const auto& base = base_of(listed);
            if (base.id.market() != MarketType::Perp) {
                continue; // the perpetual board; spot is a different pairing
            }
            const auto sym = base.id.symbol().raw;
            if (sym >= shm::k_bbo_cells) {
                continue;
            }
            Bbo quote{};
            FundingUpdate funding{};
            TickerStats stats{};
            if (src.ch.bbo->read(sym, quote) == 0 || src.ch.funding->read(sym, funding) == 0) {
                continue; // nothing has been published for it yet
            }
            (void)src.ch.ticker_stats->read(sym, stats);

            Gathered got;
            got.account = src.name;
            got.exchange_symbol = std::string{base.exchange_symbol.view()};
            got.leg = scan::Leg{.venue = src.account.venue,
                                .instrument = base.id,
                                .bid = base.ticks_to_price(quote.bid),
                                .ask = base.ticks_to_price(quote.ask),
                                .rate = funding.rate,
                                .interval_s = funding.interval_s,
                                .next_funding = funding.next_funding_time,
                                .volume_24h_quote = stats.volume_24h_quote};
            if (const auto hourly = scan::per_hour(funding.rate, funding.interval_s);
                hourly.has_value()) {
                got.rate_per_hour = *hourly;
            }
            by_symbol[std::string{base.local_symbol.view()}].push_back(std::move(got));
        }
    }

    const auto to_wire = [](const Gathered& g) {
        return WireLeg{.account = g.account,
                       .venue = std::string{to_string(g.leg.venue)},
                       .symbol = g.exchange_symbol,
                       .bid = decimal(g.leg.bid),
                       .ask = decimal(g.leg.ask),
                       .rate = decimal(g.leg.rate),
                       .rate_per_8h = decimal(FixedE9::from_raw(
                           g.rate_per_hour.is_undef() ? 0 : g.rate_per_hour.raw * 8)),
                       .interval_s = g.leg.interval_s,
                       .next_funding_time = digits(g.leg.next_funding.ns),
                       .volume_24h_quote = decimal(g.leg.volume_24h_quote)};
    };

    struct Scored {
        scan::Row row;
        const Gathered* long_side{nullptr};
        const Gathered* short_side{nullptr};
        const std::string* symbol{nullptr};
    };

    std::vector<Scored> scored;
    std::size_t considered = 0;

    for (const auto& [symbol, legs] : by_symbol) {
        for (std::size_t i = 0; i < legs.size(); ++i) {
            for (std::size_t j = i + 1; j < legs.size(); ++j) {
                ++considered;
                const auto priced = scan::pair(legs[i].leg, legs[j].leg, min_volume);
                if (!priced.has_value()) {
                    continue;
                }
                const bool i_is_short = priced->short_leg.venue == legs[i].leg.venue;
                scored.push_back({.row = *priced,
                                  .long_side = i_is_short ? &legs[j] : &legs[i],
                                  .short_side = i_is_short ? &legs[i] : &legs[j],
                                  .symbol = &symbol});
            }
        }
    }

    // Ranked on the FUNDING edge alone. The price spread is beside it for a
    // reader to weigh, never folded in — a route whose perpetuals sit far apart
    // is a directional position, and a combined score would float it to the top
    // exactly when it is least safe.
    std::ranges::sort(scored, [](const Scored& a, const Scored& b) {
        return a.row.funding_edge_8h.raw > b.row.funding_edge_8h.raw;
    });
    if (scored.size() > limit) {
        scored.resize(limit);
    }

    WireBoard board{.ts = digits(now.ns),
                    .venues = live_venues,
                    .symbols = by_symbol.size(),
                    .considered = considered,
                    .rows = {}};
    board.rows.reserve(scored.size());
    for (const auto& s : scored) {
        board.rows.push_back(WireRow{.symbol = *s.symbol,
                                     .long_leg = to_wire(*s.long_side),
                                     .short_leg = to_wire(*s.short_side),
                                     .funding_edge_8h = decimal(s.row.funding_edge_8h),
                                     .price_spread_bp = s.row.price_spread_bp,
                                     .volume_floor = decimal(s.row.volume_floor)});
    }
    return board;
}

} // namespace

int main(int argc, char** argv) {
    using namespace raikiri;

    std::uint16_t port = 8790;
    std::string shm_dir;
    std::uint64_t duration_s = 0;
    std::int64_t min_volume_whole = 0;
    std::size_t limit = 200;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--shm" && i + 1 < argc) {
            shm_dir = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--min-volume" && i + 1 < argc) {
            min_volume_whole = std::atoll(argv[++i]);
        } else if (arg == "--limit" && i + 1 < argc) {
            limit = static_cast<std::size_t>(std::atoll(argv[++i]));
        }
    }
    if (shm_dir.empty()) {
        shm_dir = platform::runtime_dir() + "/shm";
    }
    const auto min_volume = FixedE9::from_raw(min_volume_whole * 1'000'000'000LL);

    // Without this every RLOG_ line is dropped, and a process that refuses to
    // start says so to nobody.
    const log::Scope logging{"kowalski"};

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const LiveClock clock;

    // HELD BY POINTER: the channel pointers below reference mappings this
    // object owns, so a vector reallocation would leave every one of them
    // dangling.
    std::vector<std::unique_ptr<Source>> sources;

    // RESCANNED, NOT DISCOVERED ONCE. A gateway's segment file exists from the
    // instant it is created and carries nothing until it publishes, so a
    // scanner that looks once at startup sees an empty directory and gives up —
    // which is exactly what it did the first time the launcher started it
    // beside the gateways rather than after them. Rescanning also means a venue
    // brought up an hour later joins the board without restarting this.
    //
    // The generation check is what keeps a rescan from spending a reader slot
    // every two seconds; admind's registry carries the same guard for the same
    // reason, and there are only four slots.
    const auto rescan = [&sources, &shm_dir]() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{shm_dir, ec}) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("gw.")) {
                continue;
            }
            const auto account_name = name.substr(3);
            const auto account = account_from_string(account_name);
            if (account.venue == VenueId::Invalid) {
                continue;
            }
            auto client = shm::SegmentClient::attach(entry.path().string());
            if (!client.has_value()) {
                continue;
            }
            const auto held = std::ranges::find_if(
                sources, [&](const auto& s) { return s->name == account_name; });
            if (held != sources.end() && (*held)->client.generation() == client->generation()) {
                continue; // the same run we already hold
            }
            auto channels = shm::open_gateway_channels(*client);
            if (!channels.has_value()) {
                continue; // created but not yet published
            }
            if (!client->register_reader(::getpid()).has_value()) {
                RLOG_ERROR("kowalski: {} has no free reader slot (shm::k_max_readers is {})",
                           account_name,
                           shm::k_max_readers);
                continue;
            }
            if (held != sources.end()) {
                RLOG_INFO("kowalski: {} came back on a new generation", account_name);
                **held = Source{account, account_name, std::move(*client), *channels};
                continue;
            }
            RLOG_INFO("kowalski: reading {}", account_name);
            sources.push_back(
                std::make_unique<Source>(account, account_name, std::move(*client), *channels));
        }
        if (ec) {
            RLOG_WARN("kowalski: cannot read {} — {}", shm_dir, ec.message());
        }
    };
    rescan();

    // The board a request answers from, swapped whole. A reader never sees a
    // half-rebuilt board, and the rebuild never waits on a request.
    std::string served = R"({"ts":"0","venues":0,"symbols":0,"considered":0,"rows":[]})";
    std::mutex served_mu;

    http::Server server{
        "127.0.0.1", port, [&served, &served_mu](const http::Request& req) -> http::Response {
            if (req.method != "GET") {
                return {.status = 405, .content_type = "application/json", .body = "{}"};
            }
            if (req.path == "/v1/scanner/funding") {
                const std::lock_guard lock{served_mu};
                return {.status = 200, .content_type = "application/json", .body = served};
            }
            if (req.path == "/v1/health") {
                return {
                    .status = 200, .content_type = "application/json", .body = R"({"ok":true})"};
            }
            return {.status = 404, .content_type = "application/json", .body = "{}"};
        }};
    if (!server.start()) {
        RLOG_ERROR("kowalski: port {} is taken", port);
        return 1;
    }
    RLOG_INFO("kowalski: http://127.0.0.1:{}/v1/scanner/funding", server.port());

    // ONCE A SECOND, not per tick. The board is a cross product over every
    // symbol every venue lists, and recomputing it on each quote would be
    // thousands of passes a second over a page nobody reads that often.
    constexpr std::uint64_t k_rebuild_ns = 1'000'000'000;
    constexpr std::uint64_t k_rescan_ns = 2'000'000'000;
    std::uint64_t next_scan = 0;
    const auto t_end =
        duration_s != 0 ? clock.mono() + duration_s * 1'000'000'000ULL : MonoNanos{~0ULL};
    std::uint64_t next_build = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto mono = clock.mono();
        if (mono >= t_end) {
            break;
        }
        if (mono.ns >= next_scan) {
            rescan();
            next_scan = mono.ns + k_rescan_ns;
        }
        if (mono.ns >= next_build) {
            auto board = build(sources, min_volume, limit, clock.wall(), mono);
            std::string json;
            if (const auto err = glz::write_json(board, json); !err) {
                const std::lock_guard lock{served_mu};
                served = std::move(json);
            }
            next_build = mono.ns + k_rebuild_ns;
        }
        server.poll();
        // Parked for the same reason gatewayd is: a loop that spins burns a
        // core whether or not anyone is asking.
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    server.stop();
    RLOG_INFO("kowalski: down");
    return 0;
}
