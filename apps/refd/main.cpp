// refd: one reference mid per asset from every gateway's books, published into the ref.mid segment

#include <raikiri/common/platform.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/common/version.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/reference/engine.hpp>
#include <raikiri/reference/gateways.hpp>
#include <raikiri/reference/segment.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

constexpr std::string_view k_usage = R"(usage: refd [--period-ms 500] [--coins BTC,ETH,SOL,XRP,DOGE,HYPE,BNB] [--top 20]
            [--min-sources 3] [--outlier-bp 200] [--max-spread-bp 50] [--weighting volume|liquidity]
            [--duration <seconds>]
)";

constexpr std::string_view k_default_coins = "BTC,ETH,SOL,XRP,DOGE,HYPE,BNB";
constexpr std::uint64_t k_ms = 1'000'000;

[[nodiscard]] std::int64_t number(std::string_view text, std::int64_t fallback) {
    std::int64_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && stop == text.data() + text.size() ? value : fallback;
}

[[nodiscard]] std::vector<std::string> asset_keys(std::string_view coins) {
    std::vector<std::string> out;
    while (!coins.empty()) {
        const auto comma = coins.find(',');
        const auto coin = coins.substr(0, comma);
        if (!coin.empty()) {
            out.push_back(coin.find(':') == std::string_view::npos ? "crypto:" + std::string{coin} : std::string{coin});
        }
        coins = comma == std::string_view::npos ? std::string_view{} : coins.substr(comma + 1);
    }
    return out;
}

} // namespace

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
    const raikiri::log::Scope logging{"refd"};
    namespace ref = raikiri::reference;

    std::int64_t period_ms = 500;
    std::string coins{k_default_coins};
    ref::Params params{};
    ref::Universe universe{};
    std::int64_t duration_s = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view arg{argv[i]};
        const std::string_view value{argv[i + 1]};
        if (arg == "--period-ms") {
            period_ms = number(value, period_ms);
        } else if (arg == "--coins") {
            coins = value;
        } else if (arg == "--top") {
            universe.top_n = static_cast<std::size_t>(std::max<std::int64_t>(number(value, 20), 0));
        } else if (arg == "--min-sources") {
            params.min_sources = static_cast<std::size_t>(std::max<std::int64_t>(number(value, 3), 1));
        } else if (arg == "--outlier-bp") {
            params.outlier_e9 = number(value, 200) * 100'000;
        } else if (arg == "--max-spread-bp") {
            params.max_spread_e9 = number(value, 50) * 100'000;
        } else if (arg == "--weighting") {
            params.weighting = value == "liquidity" ? ref::Weighting::Liquidity : ref::Weighting::Volume;
        } else if (arg == "--duration") {
            duration_s = number(value, 0);
        } else {
            continue;
        }
        ++i;
    }
    period_ms = std::clamp<std::int64_t>(period_ms, 250, 1000);
    universe.pinned = asset_keys(coins);
    universe.min_venues = params.min_sources;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const raikiri::LiveClock clock;
    const auto mono = [&clock] { return clock.mono().ns; };
    const auto wall = [&clock] { return clock.wall().ns; };
    const auto shm_dir = raikiri::platform::runtime_dir() + "/shm";
    std::error_code ec;
    std::filesystem::create_directories(shm_dir, ec);

    const auto period_ns = static_cast<std::uint64_t>(period_ms) * k_ms;
    const auto path = ref::ref_segment_path();
    auto writer = ref::RefWriter::create(path, wall(), mono(), period_ns);
    if (!writer.has_value()) {
        RLOG_ERROR("refd: cannot create {} ({})",
                   path,
                   writer.error() == raikiri::shm::SegError::AlreadyRunning ? "another refd holds it"
                                                                            : "create failed");
        return 1;
    }
    ref::GatewayQuotes gateways{shm_dir, "refd", mono};
    ref::ComputeReferences cycle{gateways, **writer, params, universe, wall};
    RLOG_INFO("refd: publishing {} every {} ms, {} pinned and the top {} by turnover",
              path,
              period_ms,
              universe.pinned.size(),
              universe.top_n);

    constexpr std::uint64_t k_rescan_ns = 2'000 * k_ms;
    constexpr std::uint64_t k_report_ns = 60'000 * k_ms;
    constexpr auto k_tick = std::chrono::milliseconds{50};
    const auto started = mono();
    auto rescan_due = started;
    auto cycle_due = started;
    auto report_due = started + k_report_ns;
    std::size_t members = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        auto now = mono();
        if (duration_s > 0 && now - started >= static_cast<std::uint64_t>(duration_s) * 1'000 * k_ms) {
            break;
        }
        if (now >= rescan_due) {
            rescan_due = now + k_rescan_ns;
            const auto found = gateways.rescan();
            for (const auto& name : found.attached) {
                RLOG_INFO("refd: reading gw.{}", name);
            }
            for (const auto& name : found.unlisted) {
                RLOG_WARN("refd: gw.{} has no free reader slot, reading unlisted", name);
            }
            for (const auto& name : found.gone) {
                RLOG_INFO("refd: gw.{} went away", name);
            }
            (*writer)->set_gateways(static_cast<std::uint32_t>(gateways.attached()),
                                    static_cast<std::uint32_t>(gateways.unlisted()));
        }
        if (now >= cycle_due) {
            const auto report = cycle();
            cycle_due = std::max(cycle_due + period_ns, now);
            if (cycle.members().size() != members || now >= report_due) {
                members = cycle.members().size();
                report_due = now + k_report_ns;
                char usdt[48];
                char usdc[48];
                RLOG_INFO("refd: {} assets, {} refused, {} books from {} gateways, usdt {}{} usdc {}{}, {} us",
                          members,
                          report.refused,
                          report.quotes,
                          gateways.attached(),
                          raikiri::format_e9(report.rates.usdt_usd, usdt, sizeof usdt),
                          report.rates.usdt_fallback ? " (fallback)" : "",
                          raikiri::format_e9(report.rates.usdc_usd, usdc, sizeof usdc),
                          report.rates.usdc_fallback ? " (fallback)" : "",
                          report.took_ns / 1'000);
            }
        }
        (*writer)->beat(mono());
        std::this_thread::sleep_for(k_tick);
    }
    RLOG_INFO("refd: down");
    return 0;
}
