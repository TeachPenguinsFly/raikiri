// reftop: the ref.mid segment as a table

#include <raikiri/common/time.hpp>
#include <raikiri/reference/routes.hpp>
#include <raikiri/reference/segment.hpp>

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

constexpr std::string_view k_usage = "usage: reftop [--once]\n";
constexpr std::uint64_t k_ms = 1'000'000;

[[nodiscard]] std::string price(raikiri::FixedE9 value) {
    char buf[48];
    const auto text = raikiri::format_e9(value, buf, sizeof buf);
    return text.empty() ? std::string{"-"} : std::string{text};
}

[[nodiscard]] std::string bp(raikiri::FixedE9 ratio) {
    return ratio.is_undef() ? std::string{"-"} : raikiri::reference::bp_text(ratio.raw);
}

void print(const raikiri::reference::RefSnapshot& snap, std::uint64_t wall_ns) {
    namespace ref = raikiri::reference;
    const auto& m = snap.metrics;
    fmt::print("refd {} | cycle {} | {} gateways ({} unlisted) | {} books | usdt {}{} usdc {}{} | {} us\n",
               snap.owner_alive ? "alive" : "not answering",
               m.cycles,
               m.gateways,
               m.unlisted,
               m.quotes,
               price(m.usdt_usd),
               (m.flags & ref::FlagUsdtFallback) != 0 ? "*" : "",
               price(m.usdc_usd),
               (m.flags & ref::FlagUsdcFallback) != 0 ? "*" : "",
               m.took_ns / 1'000);
    fmt::print("{:<18} {:>18} {:>8} {:>8} {:>9} {:>6} {:>6} {:>8} {}\n",
               "asset",
               "mid",
               "conf bp",
               "disp bp",
               "basis bp",
               "spot",
               "perp",
               "age ms",
               "cuts");
    for (const auto& cell : snap.rows) {
        std::string cuts;
        for (std::size_t c = 0; c < ref::k_cut_count; ++c) {
            if (cell.cuts[c] != 0) {
                cuts += fmt::format("{}={} ", ref::to_string(static_cast<ref::Cut>(c)), cell.cuts[c]);
            }
        }
        const auto conf = cell.conf.is_undef() || cell.mid.is_undef() || cell.mid.raw <= 0
                              ? raikiri::FixedE9::undef()
                              : raikiri::FixedE9::from_raw(
                                    raikiri::mul_div(cell.conf.raw, raikiri::k_e9, cell.mid.raw));
        fmt::print("{:<18} {:>18} {:>8} {:>8} {:>9} {:>6} {:>6} {:>8} {}{}\n",
                   cell.asset_key(),
                   price(cell.mid),
                   bp(conf),
                   bp(cell.dispersion_e9),
                   bp(cell.basis_e9),
                   cell.used_spot,
                   cell.used_perp,
                   wall_ns > cell.ts_ns ? (wall_ns - cell.ts_ns) / k_ms : 0,
                   (cell.flags & ref::FlagRefused) != 0 ? "refused " : "",
                   cuts);
    }
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            std::fputs(k_usage.data(), stdout);
            return 0;
        }
        once = once || arg == "--once";
    }
    namespace ref = raikiri::reference;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const raikiri::LiveClock clock;
    auto reader = ref::RefReader::attach(ref::ref_segment_path(), "reftop");
    if (!reader.has_value()) {
        std::fprintf(stderr, "reftop: no ref.mid segment at %s; is refd running?\n", ref::ref_segment_path().c_str());
        return 1;
    }
    while (!g_stop.load(std::memory_order_relaxed)) {
        print(reader->read(clock.mono().ns), clock.wall().ns);
        if (once) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds{1});
        std::fputs("\n", stdout);
    }
    return 0;
}
