// Two-process SPSC ping-pong over a shared-memory segment.
//
// Parent and child each own one direction (two SpscRings in one segment).
// The parent timestamps a message, the child echoes it, and the parent
// records the round trip. Reported figures are RTT — halve for one-way hop
// cost. Both sides busy-poll; on Linux pass --cpu A --cpu-child B to pin the
// two processes to distinct cores (macOS: unpinned, numbers are indicative).
//
// This is the transport the order path rides: strategy -> gateway command
// rings are exactly these primitives.

#include <raikiri/common/platform.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/shm/segment.hpp>
#include <raikiri/shm/spsc_ring.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using raikiri::LiveClock;
using raikiri::shm::SegmentClient;
using raikiri::shm::SegmentOwner;
using raikiri::shm::SpscConsumer;
using raikiri::shm::SpscProducer;
using raikiri::shm::SpscRing;

using Ring = SpscRing<std::uint64_t, 4096>;

constexpr std::uint64_t k_warmup = 20'000;
constexpr std::uint64_t k_iters = 200'000;

void maybe_pin(int cpu) {
    if (cpu < 0) {
        return;
    }
    if (const auto r = raikiri::platform::pin_thread(static_cast<unsigned>(cpu)); !r.has_value()) {
        std::fprintf(stderr, "pin to cpu %d unavailable: %s\n", cpu, r.error().message().c_str());
    }
}

int child_main(const std::string& path, int cpu) {
    maybe_pin(cpu);
    auto client = SegmentClient::attach(path);
    if (!client.has_value()) {
        return 2;
    }
    auto ping = client->open_channel<Ring>("ping");
    auto pong = client->open_channel<Ring>("pong");
    if (!ping.has_value() || !pong.has_value()) {
        return 3;
    }
    SpscConsumer in{*ping};
    SpscProducer out{*pong};

    for (std::uint64_t received = 0; received < k_warmup + k_iters;) {
        std::uint64_t v{};
        if (in.try_pop(v)) {
            while (!out.try_push(v).has_value()) {
            }
            ++received;
        }
    }
    return 0;
}

void report(std::vector<std::uint64_t>& rtt_ns) {
    std::sort(rtt_ns.begin(), rtt_ns.end());
    const auto at = [&](double q) {
        const auto idx = static_cast<std::size_t>(q * static_cast<double>(rtt_ns.size() - 1));
        return rtt_ns[idx];
    };
    std::printf("shm spsc ping-pong (%llu round trips, one process each side)\n",
                static_cast<unsigned long long>(rtt_ns.size()));
    std::printf("  %-8s %10s %10s\n", "", "RTT", "one-way");
    const auto row = [&](const char* name, std::uint64_t v) {
        std::printf("  %-8s %8llu ns %7llu ns\n", name, static_cast<unsigned long long>(v),
                    static_cast<unsigned long long>(v / 2));
    };
    row("p50", at(0.50));
    row("p90", at(0.90));
    row("p99", at(0.99));
    row("p999", at(0.999));
    row("max", rtt_ns.back());
}

} // namespace

int main(int argc, char** argv) {
    int cpu_parent = -1;
    int cpu_child = -1;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--cpu") == 0) {
            cpu_parent = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--cpu-child") == 0) {
            cpu_child = std::atoi(argv[i + 1]);
        }
    }

    const std::string path = "/tmp/raikiri-pingpong-" + std::to_string(::getpid());
    const LiveClock clock;

    auto owner = SegmentOwner::create(path, 4U << 20, clock.mono().ns, clock.mono().ns);
    if (!owner.has_value()) {
        std::fprintf(stderr, "segment create failed\n");
        return 1;
    }
    auto* ping = owner->create_channel<Ring>("ping");
    auto* pong = owner->create_channel<Ring>("pong");
    if (ping == nullptr || pong == nullptr) {
        std::fprintf(stderr, "channel placement failed\n");
        return 1;
    }
    owner->publish();

    const pid_t child = ::fork();
    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        ::_exit(child_main(path, cpu_child));
    }

    maybe_pin(cpu_parent);
    SpscProducer out{ping};
    SpscConsumer in{pong};

    std::vector<std::uint64_t> rtt_ns;
    rtt_ns.reserve(k_iters);
    for (std::uint64_t i = 0; i < k_warmup + k_iters; ++i) {
        const auto t0 = clock.mono().ns;
        while (!out.try_push(t0).has_value()) {
        }
        std::uint64_t echoed{};
        while (!in.try_pop(echoed)) {
        }
        const auto t1 = clock.mono().ns;
        if (i >= k_warmup) {
            rtt_ns.push_back(t1 - echoed);
        }
    }

    int status = 0;
    ::waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "child failed: %d\n", status);
        return 1;
    }
    report(rtt_ns);
    return 0;
}
