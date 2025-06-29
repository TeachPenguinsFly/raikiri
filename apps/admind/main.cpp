// admind — the read side of a running raikiri deployment.
//
//   admind [--address 127.0.0.1] [--port 8787]
//
// Attaches to every gateway segment it finds and serves
// schema/admin-api.yaml over HTTP. It is a READER: every write — halting an
// instrument, cancelling an order, moving a risk limit — needs a path into a
// running strategyd, and the command rings are SPSC with strategyd as their
// single producer. A second producer is a decision to be made deliberately,
// not a POST added quietly.
//
// It binds LOOPBACK by default and there is no authentication. That is not an
// oversight to be fixed by a flag: shipping an identity story that "works for
// now" is how a terminal ends up with a static token in a repository.

#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/routes.hpp>
#include <raikiri/admin/server.hpp>
#include <raikiri/common/platform.hpp>
#include <raikiri/common/time.hpp>

#include <fmt/format.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

} // namespace

int main(int argc, char** argv) {
    using namespace raikiri;

    std::string address = "127.0.0.1";
    std::uint16_t port = 8787;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--address" && i + 1 < argc) {
            address = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;
    admin::Registry registry;
    const auto shm_dir = platform::runtime_dir() + "/shm";
    const auto found = registry.discover(shm_dir, ::getpid());
    if (found == 0) {
        // Not fatal: gateways come and go, and a terminal that can say "none
        // are running" is more useful than one that refuses to start.
        fmt::print("admind: no gateway segments under {}\n", shm_dir);
    }
    for (const auto& account : registry.accounts(clock.mono())) {
        fmt::print("admind: attached to {} — {} instruments, owner {}\n",
                   account.account,
                   account.gateway.instruments,
                   account.gateway.alive ? "alive" : "not answering");
    }

    admin::Server server{address, port,
                         admin::make_router(registry, [&clock] { return clock.mono(); })};
    if (!server.start()) {
        fmt::print(stderr, "admind: cannot bind {}:{}\n", address, port);
        return 1;
    }
    fmt::print("admind: listening on http://{}:{}\n", address, server.port());

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (server.poll() == 0) {
            // Nothing ready. This is an admin API, not a hot path: sleeping a
            // millisecond costs a millisecond of latency nobody can perceive
            // and saves a core from spinning for it.
            ::usleep(1000);
        }
    }
    server.stop();
    fmt::print("admind: down\n");
    return 0;
}
