// apid: the internet-facing client api; keys, signing, scopes and limits live here, trading stays behind admind

#include <raikiri/audit/file_sink.hpp>
#include <raikiri/clientapi/application/auth.hpp>
#include <raikiri/clientapi/application/book.hpp>
#include <raikiri/clientapi/application/desk.hpp>
#include <raikiri/clientapi/application/feed.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/infrastructure/admin_backend.hpp>
#include <raikiri/clientapi/infrastructure/edge_server.hpp>
#include <raikiri/clientapi/infrastructure/stores.hpp>
#include <raikiri/clientapi/presentation/router.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/common/version.hpp>
#include <raikiri/config/secret.hpp>
#include <raikiri/log/log.hpp>

#include <openssl/rand.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_reload{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

void on_hangup(int) {
    g_reload.store(true, std::memory_order_relaxed);
}

int print_new_secret() {
    std::array<unsigned char, 32> raw{};
    if (::RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        return 1;
    }
    for (const auto byte : raw) {
        std::printf("%02x", static_cast<unsigned>(byte));
    }
    std::printf("\n");
    return 0;
}

constexpr std::string_view k_usage = R"(usage: apid --keys <file> --state-dir <dir>
            [--address 127.0.0.1] [--port 8900] [--workers 4]
            [--admin-host 127.0.0.1] [--admin-port 8787] [--admin-stream-port 8788]
            [--skew-ms 5000] [--trust-proxy <cidr>]... [--audit <dir>] [--plaintext-ok]
       apid --new-secret
SIGHUP reloads the key file. apid speaks plain http: put a tls terminator in front, and bind
anything but loopback only with --plaintext-ok.
)";

} // namespace

int main(int argc, char** argv) {
    namespace config = raikiri::config;
    namespace log = raikiri::log;
    using raikiri::k_version;
    using raikiri::LiveClock;
    using raikiri::clientapi::AdminBackend;
    using raikiri::clientapi::ApiRouter;
    using raikiri::clientapi::Authenticator;
    using raikiri::clientapi::AuthPolicy;
    using raikiri::clientapi::ClientBook;
    using raikiri::clientapi::ClientDesk;
    using raikiri::clientapi::EdgeOptions;
    using raikiri::clientapi::EdgeServer;
    using raikiri::clientapi::FileLedger;
    using raikiri::clientapi::InstrumentDirectory;
    using raikiri::clientapi::KeyFileDirectory;
    using raikiri::clientapi::parse_ip_range;
    using raikiri::clientapi::PrivateFeed;
    using raikiri::clientapi::StreamHub;
    using raikiri::clientapi::WallClock;

    std::string keys_path;
    std::string state_dir;
    std::string audit_path;
    EdgeOptions options{};
    std::uint16_t admin_port = 8787;
    AuthPolicy policy{};
    bool plaintext_ok = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--help" || arg == "-h") {
            std::fputs(k_usage.data(), stdout);
            return 0;
        }
        if (arg == "--version") {
            std::printf("raikiri %.*s\n", static_cast<int>(k_version.size()), k_version.data());
            return 0;
        }
        if (arg == "--new-secret") {
            return print_new_secret();
        }
        if (arg == "--keys") {
            keys_path = next();
        } else if (arg == "--state-dir") {
            state_dir = next();
        } else if (arg == "--audit") {
            audit_path = next();
        } else if (arg == "--address") {
            options.address = next();
        } else if (arg == "--port") {
            options.port = static_cast<std::uint16_t>(std::atoi(next()));
        } else if (arg == "--workers") {
            options.workers = static_cast<std::size_t>(std::atoi(next()));
        } else if (arg == "--admin-host") {
            options.admin_host = next();
        } else if (arg == "--admin-port") {
            admin_port = static_cast<std::uint16_t>(std::atoi(next()));
        } else if (arg == "--admin-stream-port") {
            options.admin_stream_port = static_cast<std::uint16_t>(std::atoi(next()));
        } else if (arg == "--skew-ms") {
            policy.skew_ms = static_cast<std::uint64_t>(std::atoll(next()));
        } else if (arg == "--plaintext-ok") {
            plaintext_ok = true;
        } else if (arg == "--trust-proxy") {
            const auto range = parse_ip_range(next());
            if (!range.has_value()) {
                std::fprintf(stderr, "apid: --trust-proxy takes an address or a cidr\n");
                return 2;
            }
            options.trusted_proxies.push_back(*range);
        } else {
            std::fprintf(stderr, "apid: unknown argument %s\n", argv[i]);
            std::fputs(k_usage.data(), stderr);
            return 2;
        }
    }
    if (keys_path.empty() || state_dir.empty()) {
        std::fputs(k_usage.data(), stderr);
        return 2;
    }
    if (!plaintext_ok && options.address != "127.0.0.1" && options.address != "::1") {
        std::fprintf(stderr,
                     "apid: %s is not loopback and apid has no tls; use a terminator, or --plaintext-ok\n",
                     options.address.c_str());
        return 2;
    }
    const log::Scope logging{"apid"};
    config::disable_core_dumps();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP, on_hangup);

    std::error_code made;
    std::filesystem::create_directories(state_dir, made);
    if (made) {
        RLOG_ERROR("apid: cannot create {}", state_dir);
        return 1;
    }

    config::DefaultSecretProvider secrets;
    KeyFileDirectory keys{keys_path, secrets};
    const auto loaded = keys.load();
    if (!loaded.has_value()) {
        RLOG_ERROR("apid: {}", loaded.error());
        return 2;
    }
    RLOG_INFO("apid: {} keys from {}", *loaded, keys_path);

    FileLedger ledger{std::filesystem::path{state_dir} / "ledger.jsonl"};
    if (!ledger.open()) {
        RLOG_ERROR("apid: cannot open the ledger in {}", state_dir);
        return 1;
    }
    const LiveClock clock;
    auto audit = raikiri::audit::FileSink::open(
        audit_path.empty() ? (std::filesystem::path{state_dir} / "audit").string() : audit_path, {}, clock);
    if (!audit.has_value()) {
        RLOG_ERROR("apid: cannot open the audit log: {}", raikiri::audit::to_string(audit.error()));
        return 1;
    }

    const WallClock wall = [&clock] { return clock.wall(); };
    constexpr std::uint64_t k_idempotency_ttl_ns = 86'400ULL * 1'000'000'000ULL;
    constexpr std::size_t k_ring_events = 65'536;

    AdminBackend backend{options.admin_host, admin_port};
    ClientBook book{ledger, k_idempotency_ttl_ns};
    book.load(wall().ns);
    StreamHub hub{std::to_string(wall().ns), k_ring_events};
    InstrumentDirectory instruments{backend, wall};
    ClientDesk desk{
        backend, book, instruments, wall, [](std::chrono::milliseconds pause) { std::this_thread::sleep_for(pause); }};
    PrivateFeed feed{backend, book, hub, instruments, wall};
    Authenticator auth{keys, wall, policy};
    ApiRouter router{auth, desk, hub, **audit, book, wall, &backend, keys.extensions()};
    EdgeServer server{options, router, backend, hub, feed, wall};
    if (!server.start()) {
        RLOG_ERROR("apid: cannot bind {}:{}", options.address, options.port);
        return 1;
    }
    RLOG_INFO("apid: listening on {}:{}", options.address, server.port());

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        if (g_reload.exchange(false)) {
            const auto again = keys.load();
            if (again.has_value()) {
                RLOG_INFO("apid: reloaded {} keys", *again);
            } else {
                RLOG_ERROR("apid: keeping the keys in force; {}", again.error());
            }
        }
    }
    server.stop();
    RLOG_INFO("apid: down");
    return 0;
}
