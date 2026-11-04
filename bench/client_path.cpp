// apid without its socket: a signed request through verify and route, and the private stream's fanout

#include <raikiri/clientapi/application/auth.hpp>
#include <raikiri/clientapi/application/book.hpp>
#include <raikiri/clientapi/application/desk.hpp>
#include <raikiri/clientapi/application/feed.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/presentation/frames.hpp>
#include <raikiri/clientapi/presentation/router.hpp>
#include <raikiri/common/sanitizer.hpp>

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "fakes.hpp"

namespace {

thread_local std::uint64_t t_allocs = 0;

} // namespace

#if !RAIKIRI_TSAN
// gcc pairs this malloc with the libc free below and reads a mismatch once they inline together
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n); p != nullptr) {
        return p;
    }
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
#endif

namespace {

using namespace raikiri;
using namespace raikiri::clientapi;

constexpr std::uint64_t k_start_ns = 1'790'000'000'000'000'000ULL;

class NoAudit final : public AuditSink {
public:
    void append(const AuditEntry&) override {}
};

struct Rig {
    Rig()
        : auth{keys, wall, {.skew_ms = 5'000'000}}, book{ledger, 86'400ULL * 1'000'000'000ULL}, hub{"e1", 4096},
          instruments{backend, wall},
          desk{backend,
               book,
               instruments,
               wall,
               [](std::chrono::milliseconds) {},
               {.wait = std::chrono::milliseconds{100}, .poll = std::chrono::milliseconds{50}}},
          router{auth, desk, hub, audit, book, wall} {
        backend.listings["1"] = test::listing("1", "binance", "BTC/USDT");
        auto key = test::full_key();
        key.rate = {.requests_per_second = 1'000'000'000,
                    .burst = 1'000'000'000,
                    .mutations_per_second = 1'000'000'000,
                    .mutation_burst = 1'000'000'000};
        keys.add(std::move(key));
    }

    EdgeResponse call(std::string_view method, std::string_view path, std::string_view body, std::uint64_t salt) {
        now_ns += 1'000'000;
        const auto ts = std::to_string(now_ns / 1'000'000ULL);
        const auto signature = test::sign(canonical_request(ts, method, path, {}, body));
        const auto idem = method == "GET" ? std::string{} : "idem-key-" + std::to_string(salt);
        const EdgeRequest request{.method = method,
                                  .path = path,
                                  .body = body,
                                  .key_id = "ak1",
                                  .timestamp = ts,
                                  .signature = signature,
                                  .idempotency_key = idem,
                                  .peer_ip = "203.0.113.5"};
        return router.handle(request);
    }

    std::uint64_t now_ns{k_start_ns};
    WallClock wall = [this] { return UnixNanos{now_ns}; };
    test::FakeKeys keys;
    test::FakeBackend backend;
    test::MemoryLedger ledger;
    NoAudit audit;
    Authenticator auth;
    ClientBook book;
    StreamHub hub;
    InstrumentDirectory instruments;
    ClientDesk desk;
    ApiRouter router;
};

void count_allocs(benchmark::State& state, std::uint64_t before, std::uint64_t ops) {
    state.counters["allocs"] = benchmark::Counter(static_cast<double>(t_allocs - before) / static_cast<double>(ops));
}

void bm_sign(benchmark::State& state) {
    const std::string message(200, 'x');
    for (auto _ : state) {
        benchmark::DoNotOptimize(test::sign(message));
    }
}

void bm_get_route(benchmark::State& state) {
    Rig rig;
    std::uint64_t salt = 0;
    const auto before = t_allocs;
    for (auto _ : state) {
        auto reply = rig.call("GET", "/api/v1/instruments/1", {}, ++salt);
        benchmark::DoNotOptimize(reply.status);
        if (reply.status != 200) {
            state.SkipWithError(reply.body.c_str());
            break;
        }
    }
    count_allocs(state, before, salt);
}

void bm_place_route(benchmark::State& state) {
    Rig rig;
    std::uint64_t salt = 0;
    const auto before = t_allocs;
    for (auto _ : state) {
        auto reply = rig.call(
            "POST",
            "/api/v1/orders",
            R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})",
            ++salt);
        benchmark::DoNotOptimize(reply.status);
        if (reply.status != 202) {
            state.SkipWithError(reply.body.c_str());
            break;
        }
        state.PauseTiming();
        rig.backend.placed.clear();
        rig.backend.keys_seen.clear();
        rig.ledger.records.clear();
        state.ResumeTiming();
    }
    count_allocs(state, before, salt);
}

class Session final : public StreamSubscriber {
public:
    void deliver(std::shared_ptr<const PrivateEvent> event) override {
        // the session's own thread does this after a post, once per session
        if (encode) {
            frames += private_frame(*event).size();
        }
        last = std::move(event);
    }

    std::shared_ptr<const PrivateEvent> last;
    std::size_t frames{0};
    bool encode{false};
};

OrderEventRow row() {
    OrderEventRow out{};
    return out;
}

void fanout(benchmark::State& state, bool encode) {
    StreamHub hub{"e1", 4096};
    std::vector<std::shared_ptr<Session>> sessions;
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        auto session = std::make_shared<Session>();
        session->encode = encode;
        hub.attach("acme", session);
        sessions.push_back(std::move(session));
    }
    std::uint64_t ops = 0;
    const auto before = t_allocs;
    for (auto _ : state) {
        hub.publish("acme", mask_of(Channel::Orders), "binance", "BTC/USDT", row());
        ++ops;
    }
    count_allocs(state, before, ops);
}

void bm_fanout_one_client(benchmark::State& state) {
    fanout(state, false);
}

void bm_fanout_one_client_encoded(benchmark::State& state) {
    fanout(state, true);
}

void bm_fanout_each_own_client(benchmark::State& state) {
    StreamHub hub{"e1", 4096};
    std::vector<std::shared_ptr<Session>> sessions;
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        auto session = std::make_shared<Session>();
        hub.attach("client-" + std::to_string(i), session);
        sessions.push_back(std::move(session));
    }
    std::uint64_t ops = 0;
    const auto before = t_allocs;
    for (auto _ : state) {
        hub.publish_connected(mask_of(Channel::Balances), "binance", row());
        ++ops;
    }
    count_allocs(state, before, ops);
}

} // namespace

BENCHMARK(bm_sign)->Unit(benchmark::kNanosecond);
BENCHMARK(bm_get_route)->Unit(benchmark::kNanosecond);
BENCHMARK(bm_place_route)->Unit(benchmark::kNanosecond);
BENCHMARK(bm_fanout_one_client)->Unit(benchmark::kMicrosecond)->Arg(1000);
BENCHMARK(bm_fanout_one_client_encoded)->Unit(benchmark::kMicrosecond)->Arg(1000);
BENCHMARK(bm_fanout_each_own_client)->Unit(benchmark::kMicrosecond)->Arg(1000);
BENCHMARK_MAIN();
