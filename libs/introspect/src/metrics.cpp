#include <raikiri/introspect/routes.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace raikiri::introspect {

namespace {

void escaped(std::string& out, std::string_view value) {
    for (const char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out += c;
        }
    }
}

using Label = std::pair<std::string_view, std::string_view>;

void sample(std::string& out, std::string_view name, std::initializer_list<Label> labels, double value) {
    out += name;
    char sep = '{';
    for (const auto& [key, val] : labels) {
        out += sep;
        out += key;
        out += "=\"";
        escaped(out, val);
        out += '"';
        sep = ',';
    }
    if (sep == ',') {
        out += '}';
    }
    std::format_to(std::back_inserter(out), " {}\n", value);
}

void family(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
    std::format_to(std::back_inserter(out), "# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
}

double seconds(std::uint64_t ns) {
    return static_cast<double>(ns) / 1e9;
}

double seconds(std::int64_t ns) {
    return static_cast<double>(ns) / 1e9;
}

bool reporting(const GatewayView& view) {
    return view.state != Liveness::Down;
}

} // namespace

std::string prometheus_text(const MachineStats& machine) {
    std::string out;
    out.reserve(4096 + machine.gateways.size() * 4096);

    family(out, "raikiri_gateway_up", "gauge", "1 while the gateway's snapshot is fresh");
    for (const auto& view : machine.gateways) {
        const auto& s = view.stats;
        sample(out,
               "raikiri_gateway_up",
               {{"account", s.account}, {"venue", s.venue}},
               view.state == Liveness::Up ? 1.0 : 0.0);
    }
    family(out, "raikiri_gateway_messages_per_second", "gauge", "Market-data messages a second");
    for (const auto& view : machine.gateways) {
        if (reporting(view)) {
            const auto& s = view.stats;
            sample(out,
                   "raikiri_gateway_messages_per_second",
                   {{"account", s.account}, {"venue", s.venue}},
                   s.rates.messages_per_second);
        }
    }

    struct ClockGauge {
        std::string_view name;
        std::string_view help;
        double (*read)(const ClockStats&, std::uint64_t now_ns);
    };

    const std::array<ClockGauge, 4> clock_gauges{{
        {"raikiri_clock_offset_seconds",
         "Venue clock minus ours, measured against its server-time method",
         [](const ClockStats& c, std::uint64_t) { return seconds(c.offset_ns); }},
        {"raikiri_clock_uncertainty_seconds",
         "Half the probe round trip plus half the stamp resolution",
         [](const ClockStats& c, std::uint64_t) { return seconds(c.uncertainty_ns); }},
        {"raikiri_clock_rtt_seconds",
         "Round trip of the probe the offset was taken from",
         [](const ClockStats& c, std::uint64_t) { return seconds(c.rtt_ns); }},
        {"raikiri_clock_sample_age_seconds",
         "Age of the probe the offset was taken from",
         [](const ClockStats& c, std::uint64_t now) { return now > c.sampled_ns ? seconds(now - c.sampled_ns) : 0.0; }},
    }};
    for (const auto& gauge : clock_gauges) {
        family(out, gauge.name, "gauge", gauge.help);
        for (const auto& view : machine.gateways) {
            const auto& s = view.stats;
            if (!reporting(view) || s.clock.method == "none") {
                continue;
            }
            sample(out,
                   gauge.name,
                   {{"account", s.account}, {"venue", s.venue}, {"method", s.clock.method}},
                   gauge.read(s.clock, machine.taken_ns));
        }
    }

    family(out,
           "raikiri_latency_seconds",
           "histogram",
           "Cumulative since the gateway started, five buckets a decade from 1 us to 10 s");
    std::array<std::string, k_bucket_bounds_ns.size()> bounds;
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        bounds[i] = std::format("{}", seconds(k_bucket_bounds_ns[i]));
    }
    for (const auto& view : machine.gateways) {
        if (!reporting(view)) {
            continue;
        }
        const auto& s = view.stats;
        for (const auto& l : s.latency) {
            const auto bucket = [&](std::string_view le, std::uint64_t count) {
                sample(out,
                       "raikiri_latency_seconds_bucket",
                       {{"account", s.account},
                        {"venue", s.venue},
                        {"channel", l.channel},
                        {"stage", l.stage},
                        {"le", le}},
                       static_cast<double>(count));
            };
            if (l.buckets.size() == bounds.size()) {
                for (std::size_t i = 0; i < bounds.size(); ++i) {
                    bucket(bounds[i], l.buckets[i]);
                }
            }
            bucket("+Inf", l.total);
            const std::initializer_list<Label> labels{
                {"account", s.account}, {"venue", s.venue}, {"channel", l.channel}, {"stage", l.stage}};
            sample(out, "raikiri_latency_seconds_sum", labels, seconds(l.sum_ns));
            sample(out, "raikiri_latency_seconds_count", labels, static_cast<double>(l.total));
        }
    }
    const auto per_series = [&](std::string_view name, auto value) {
        for (const auto& view : machine.gateways) {
            if (!reporting(view)) {
                continue;
            }
            const auto& s = view.stats;
            for (const auto& l : s.latency) {
                sample(out,
                       name,
                       {{"account", s.account}, {"venue", s.venue}, {"channel", l.channel}, {"stage", l.stage}},
                       value(l));
            }
        }
    };
    family(out, "raikiri_latency_max_seconds", "gauge", "Largest value in the trailing window");
    per_series("raikiri_latency_max_seconds", [](const LatencyStats& l) { return seconds(l.max_ns); });
    family(out, "raikiri_latency_below_zero_total", "counter", "Values clamped up to zero");
    per_series("raikiri_latency_below_zero_total",
               [](const LatencyStats& l) { return static_cast<double>(l.below_zero); });
    return out;
}

} // namespace raikiri::introspect
