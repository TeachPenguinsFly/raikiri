// auditcheck: verifies a stream's chain and names the first break; exit 0 intact, 1 broken, 2 usage

#include <raikiri/audit/verify.hpp>
#include <raikiri/common/version.hpp>

#include <fmt/format.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view k_usage = R"(usage: auditcheck <stream-dir> [YYYY-MM-DD | --all]
  verifies one day, the newest when none is named, or every day from the first
)";

void print(const raikiri::audit::ChainReport& report) {
    using namespace raikiri::audit;
    if (!report.found) {
        fmt::print("{}  no such day\n", day_name(report.day));
        return;
    }
    fmt::print("{}  lines={} seq={}..{} head={} anchored={} head_checked={} recoveries={} overflows={}",
               day_name(report.day),
               report.lines,
               report.first_seq,
               report.last_seq,
               report.lines == 0 ? std::string{"-"} : to_hex(report.head),
               report.anchored,
               report.head_checked,
               report.recoveries,
               report.overflows);
    if (report.torn_tail_bytes != 0) {
        fmt::print(" torn_tail_bytes={}", report.torn_tail_bytes);
    }
    if (report.first_break.has_value()) {
        const auto& at = *report.first_break;
        fmt::print("\n  BROKEN at line {}: {} (expected seq {}, found {})\n",
                   at.line,
                   to_string(at.kind),
                   at.expected_seq,
                   at.found_seq);
    } else {
        fmt::print("  OK\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace raikiri::audit;
    if (argc >= 2 && (std::string_view{argv[1]} == "--help" || std::string_view{argv[1]} == "-h")) {
        fmt::print("{}", k_usage);
        return 0;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "--version") {
        fmt::print("raikiri {}\n", raikiri::k_version);
        return 0;
    }
    if (argc < 2 || argc > 3) {
        fmt::print(stderr, "{}", k_usage);
        return 2;
    }
    const std::string dir{argv[1]};
    const std::string_view which = argc == 3 ? std::string_view{argv[2]} : std::string_view{};
    std::vector<ChainReport> reports;
    if (which == "--all") {
        reports = verify_all(dir);
    } else if (!which.empty()) {
        const auto day = parse_day(which);
        if (!day.has_value()) {
            fmt::print(stderr, "{}", k_usage);
            return 2;
        }
        reports.push_back(verify_day(dir, *day));
    } else if (const auto days = list_days(dir); !days.empty()) {
        reports.push_back(verify_day(dir, days.back()));
    }
    if (reports.empty()) {
        fmt::print(stderr, "auditcheck: no audit days under {}\n", dir);
        return 2;
    }
    bool ok = true;
    for (const auto& report : reports) {
        print(report);
        ok = ok && report.found && report.ok();
    }
    return ok ? 0 : 1;
}
