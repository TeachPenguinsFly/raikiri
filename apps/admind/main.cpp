// admind: http and websocket view of a running deployment; binds loopback, and only the stream checks a token

#include <raikiri/admin/history.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/routes.hpp>
#include <raikiri/admin/stream.hpp>
#include <raikiri/admin/stream_token.hpp>
#include <raikiri/common/platform.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/config/secret.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/introspect/files.hpp>
#include <raikiri/introspect/routes.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/reference/routes.hpp>
#include <raikiri/reference/segment.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
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
              [--stream-secret env:NAME|file:PATH|exec:CMD]
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
    // name -> loopback port, proxied under /v1/x/<name>/
    std::map<std::string, std::uint16_t> upstreams;
    std::uint16_t stream_port = 0;
    std::string dsn;
    std::string stream_secret_ref;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--address" && i + 1 < argc) {
            address = argv[++i];
        } else if (arg == "--upstream" && i + 1 < argc) {
            const std::string_view spec{argv[++i]};
            if (const auto eq = spec.find('='); eq != std::string_view::npos) {
                upstreams.emplace(std::string{spec.substr(0, eq)},
                                  static_cast<std::uint16_t>(std::atoi(std::string{spec.substr(eq + 1)}.c_str())));
            }
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--stream-port" && i + 1 < argc) {
            stream_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--dsn" && i + 1 < argc) {
            dsn = argv[++i];
        } else if (arg == "--stream-secret" && i + 1 < argc) {
            stream_secret_ref = argv[++i];
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

    auto stream_tokens = std::make_unique<admin::StreamTokens>();
    if (stream_secret_ref.empty()) {
        if (!stream_tokens->randomize()) {
            RLOG_ERROR("admind: cannot draw a random stream secret");
            return 1;
        }
        RLOG_INFO("admind: stream tokens signed with a per-process secret");
    } else {
        const auto ref = config::SecretRef::parse(stream_secret_ref);
        if (!ref.has_value() || config::is_plaintext(ref->scheme)) {
            RLOG_ERROR("admind: --stream-secret takes an env:, file: or exec: reference, not a value");
            return 2;
        }
        config::DefaultSecretProvider provider;
        if (const auto resolved = provider.resolve(*ref, stream_tokens->secret());
            !resolved.has_value() || stream_tokens->secret().size() < 16) {
            RLOG_ERROR("admind: stream secret from {} unusable ({})",
                       config::to_string(ref->scheme),
                       resolved.has_value() ? "shorter than 16 bytes" : config::to_string(resolved.error()));
            return 2;
        }
        RLOG_INFO("admind: stream secret from {}", config::to_string(ref->scheme));
    }

    const LiveClock clock;
    admin::Registry registry;
    const auto shm_dir = platform::runtime_dir() + "/shm";
    std::set<std::string> warned;
    // a second admind on the same gateways must not write order_intents beside the first
    const auto warn_refused = [&registry, &warned] {
        for (std::size_t i = 0; i < registry.size(); ++i) {
            const auto* gw = registry.at(i);
            if (gw == nullptr || warned.contains(gw->name)) {
                continue;
            }
            if (!gw->lease.has_value()) {
                RLOG_WARN("admind: another process produces order_intents on {}; manual orders there are refused",
                          gw->name);
                warned.insert(gw->name);
            } else if (!gw->client.reader_listed()) {
                RLOG_WARN("admind: {} has no free reader slot; reading unlisted", gw->name);
                warned.insert(gw->name);
            }
        }
    };
    auto attached = registry.discover(shm_dir, ::getpid());
    warn_refused();
    if (attached == 0) {
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

    // one desk per process; two would mint colliding client order ids
    admin::ManualDesk desk{registry};

    const introspect::FileStatsSource stats_source{introspect::stats_dir(platform::runtime_dir())};
    const auto wall_ns = [&clock] { return clock.wall().ns; };
    const introspect::GetMachineStats machine_stats{stats_source, wall_ns};
    const introspect::GetGatewayStats gateway_stats{stats_source, wall_ns};
    const introspect::CacheRoutes cache{machine_stats, gateway_stats};
    const introspect::MetricsRoute metrics{machine_stats};
    const admin::StreamTokenRoute token_route{*stream_tokens, [&clock] { return clock.wall(); }};
    reference::ShmReferenceFeed reference_feed{
        reference::ref_segment_path(), "admind", [&clock] { return clock.mono().ns; }};
    const reference::ReferenceRoute reference_route{reference_feed, wall_ns};

    net::IoContext io;
    http::Server server{io,
                        address,
                        port,
                        [cache,
                         metrics,
                         token_route,
                         reference_route,
                         routes = admin::make_router(
                             registry,
                             history.get(),
                             &desk,
                             [&clock] { return clock.mono(); },
                             [&clock] { return clock.wall(); },
                             upstreams)](const http::Request& req) {
                            if (auto answer = cache(req); answer.has_value()) {
                                return std::move(*answer);
                            }
                            if (auto answer = metrics(req); answer.has_value()) {
                                return std::move(*answer);
                            }
                            if (auto answer = token_route(req); answer.has_value()) {
                                return std::move(*answer);
                            }
                            if (auto answer = reference_route(req); answer.has_value()) {
                                return std::move(*answer);
                            }
                            return routes(req);
                        }};
    if (!server.start()) {
        RLOG_ERROR("admind: cannot bind {}:{}", address, port);
        return 1;
    }
    RLOG_INFO("admind: listening on http://{}:{}", address, server.port());

    admin::Stream stream{io, registry, clock, *stream_tokens, address, stream_port};
    if (!stream.start()) {
        RLOG_ERROR("admind: cannot bind stream on {}:{}", address, stream_port);
        return 1;
    }
    RLOG_INFO("admind: stream on ws://{}:{}/v1/stream", address, stream.port());

    constexpr std::uint64_t k_rescan_ns = 2ULL * 1'000'000'000ULL;
    constexpr std::uint64_t k_idle_ns = 250ULL * 1'000'000ULL;
    auto rescan_due = clock.mono().ns;

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (const auto mono = clock.mono().ns; mono >= rescan_due) {
            rescan_due = mono + k_rescan_ns;
            const auto found = registry.discover(shm_dir, ::getpid());
            warn_refused();
            if (found > attached) {
                for (const auto& account : registry.accounts(clock.mono(), clock.wall())) {
                    RLOG_INFO("admind: attached to {} — {} instruments", account.account, account.gateway.instruments);
                }
                attached = found;
            }
        }
        (void)stream.poll();
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
