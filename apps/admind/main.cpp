// admind — the read side of a running raikiri deployment.
//
//   admind [--address 127.0.0.1] [--port 8787] [--stream-port 8788]
//          [--upstream <name>=<port>]...
//          [--dsn <postgres-dsn>]
//
// Attaches to every gateway segment it finds and serves
// schema/admin-api.yaml: HTTP on --port, the live WebSocket on --stream-port.
// Two ports rather than one because an upgrade served from the HTTP acceptor
// needs a hand-off Server does not have; when it does, the second port goes
// away without the protocol changing.
//
// It is a READER: every write — halting an instrument, cancelling an order,
// moving a risk limit — needs a path into a running strategyd, and the command
// rings are SPSC with strategyd as their single producer. A second producer is
// a decision to be made deliberately, not a POST added quietly.
//
// The DSN falls back to $RAIKIRI_PG_DSN, and is OPTIONAL: without it the live
// half still serves and every path that reads Postgres answers 503. Starting
// without a database is a normal way to run this — refusing to start would
// take the gateway view down with the history.
//
// It binds LOOPBACK by default and there is no authentication. That is not an
// oversight to be fixed by a flag: shipping an identity story that "works for
// now" is how a terminal ends up with a static token in a repository.

#include <raikiri/admin/history.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/routes.hpp>
#include <raikiri/admin/stream.hpp>
#include <raikiri/common/platform.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

} // namespace

constexpr std::string_view k_usage = R"(usage: admind [--address 127.0.0.1] [--port 8787] [--stream-port 8788]
              [--upstream <name>=<port>]...
              [--dsn <postgres-dsn>]
)";

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (const std::string_view arg{argv[i]}; arg == "--help" || arg == "-h") {
            std::fputs(k_usage.data(), stdout);
            return 0;
        }
    }
    const raikiri::log::Scope logging{"admind"};
    using namespace raikiri;

    std::string address = "127.0.0.1";
    std::uint16_t port = 8787;
    // name -> loopback port. Empty means this admind proxies nothing, and every
    // /v1/x/ path is a 404.
    std::map<std::string, std::uint16_t> upstreams;
    std::uint16_t stream_port = 0;
    std::string dsn;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--address" && i + 1 < argc) {
            address = argv[++i];
        } else if (arg == "--upstream" && i + 1 < argc) {
            // name=port. A service this repository knows nothing about, whose
            // answers are forwarded under /v1/x/<name>/.
            const std::string_view spec{argv[++i]};
            if (const auto eq = spec.find('='); eq != std::string_view::npos) {
                upstreams.emplace(std::string{spec.substr(0, eq)},
                                  static_cast<std::uint16_t>(
                                      std::atoi(std::string{spec.substr(eq + 1)}.c_str())));
            }
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--stream-port" && i + 1 < argc) {
            stream_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--dsn" && i + 1 < argc) {
            dsn = argv[++i];
        }
    }
    if (stream_port == 0) {
        stream_port = static_cast<std::uint16_t>(port + 1);
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("RAIKIRI_PG_DSN"); env != nullptr) {
            dsn = env;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;
    admin::Registry registry;
    const auto shm_dir = platform::runtime_dir() + "/shm";
    auto attached = registry.discover(shm_dir, ::getpid());
    if (attached == 0) {
        // Not fatal, and not final: the loop below rescans, so a gateway that
        // is still starting — or started tomorrow — is picked up rather than
        // missed for the life of the process.
        RLOG_INFO("admind: no gateway segments under {} yet", shm_dir);
    }
    for (const auto& account : registry.accounts(clock.mono(), clock.wall())) {
        RLOG_INFO("admind: attached to {} — {} instruments, owner {}",
                  account.account,
                  account.gateway.instruments,
                  account.gateway.alive ? "alive" : "not answering");
    }

    std::unique_ptr<admin::History> history;
    if (dsn.empty()) {
        RLOG_WARN("admind: no DSN — history paths will answer 503");
    } else {
        history = std::make_unique<admin::History>(dsn);
        history->set_catalogue([&registry](InstrumentId id) -> admin::Scales {
            const auto row = registry.catalogue_row(id);
            if (!row.has_value()) {
                return {};
            }
            const auto& base = base_of(*row->instrument);
            return {.tick_size = base.tick_size, .lot_size = base.lot_size};
        });
        if (history->connected()) {
            RLOG_INFO("admind: postgres reachable");
        } else {
            RLOG_WARN("admind: postgres not answering, will retry per request");
        }
    }

    // One desk for the process. It mints client order ids and remembers
    // idempotency keys, so it has to outlive a request and be the only thing
    // doing either — two would mint colliding ids.
    admin::ManualDesk desk{registry};

    net::IoContext io;
    http::Server server{io,
                        address,
                        port,
                        admin::make_router(
                            registry,
                            history.get(),
                            &desk,
                            [&clock] { return clock.mono(); },
                            [&clock] { return clock.wall(); },
                            upstreams)};
    if (!server.start()) {
        RLOG_ERROR("admind: cannot bind {}:{}", address, port);
        return 1;
    }
    RLOG_INFO("admind: listening on http://{}:{}", address, server.port());

    admin::Stream stream{io, registry, clock, address, stream_port};
    if (!stream.start()) {
        // Fatal, unlike a missing database: a terminal that silently has no
        // live half looks like a terminal whose markets have stopped moving.
        RLOG_ERROR("admind: cannot bind stream on {}:{}", address, stream_port);
        return 1;
    }
    RLOG_INFO("admind: stream on ws://{}:{}/v1/stream", address, stream.port());

    // Gateways come and go, and one starting a second after this process is a
    // gateway a single scan would never see again. Two seconds is far below
    // the pace anyone starts one at and far above the cost of a directory
    // listing.
    constexpr std::uint64_t k_rescan_ns = 2ULL * 1'000'000'000ULL;
    constexpr std::uint64_t k_idle_ns = 250ULL * 1'000'000ULL;
    auto rescan_due = clock.mono().ns;

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (const auto mono = clock.mono().ns; mono >= rescan_due) {
            rescan_due = mono + k_rescan_ns;
            if (const auto found = registry.discover(shm_dir, ::getpid()); found > attached) {
                for (const auto& account : registry.accounts(clock.mono(), clock.wall())) {
                    RLOG_INFO("admind: attached to {} — {} instruments",
                              account.account,
                              account.gateway.instruments);
                }
                attached = found;
            }
        }
        (void)stream.poll();
        // Both sockets share `io`, so this wakes for a request or a stream
        // client at once and otherwise sleeps until a feed is due.
        const auto mono = clock.mono().ns;
        const auto until = std::min({stream.next_due().ns, rescan_due, mono + k_idle_ns});
        if (until > mono) {
            (void)server.run_for(std::chrono::nanoseconds{until - mono});
        }
    }
    stream.stop();
    server.stop();
    RLOG_INFO("admind: down");
    return 0;
}
