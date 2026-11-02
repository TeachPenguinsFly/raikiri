#include <raikiri/clientapi/application/auth.hpp>
#include <raikiri/clientapi/application/book.hpp>
#include <raikiri/clientapi/application/desk.hpp>
#include <raikiri/clientapi/application/feed.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/presentation/frames.hpp>
#include <raikiri/clientapi/presentation/router.hpp>
#include <raikiri/domain/refusal_codes.hpp>

#include <gtest/gtest.h>

#include <atomic>

#include "fakes.hpp"

namespace raikiri::clientapi {
namespace {

using test::FakeBackend;
using test::FakeKeys;
using test::full_key;
using test::MemoryAudit;
using test::MemoryLedger;
using test::sign;

constexpr std::uint64_t k_start_ns = 1'790'000'000'000'000'000ULL;

class FakeExtension final : public ExtensionClient {
public:
    std::expected<RawReply, BackendFailure>
    call(std::uint16_t, std::string_view method, std::string_view target, std::string_view body) override {
        calls.push_back(std::string{method} + " " + std::string{target} + " " + std::string{body});
        if (const auto found = replies.find(std::string{method} + " " + std::string{target}); found != replies.end()) {
            return found->second;
        }
        return RawReply{.status = 404, .body = R"({"title":"nope"})"};
    }

    std::vector<std::string> calls;
    std::map<std::string, RawReply> replies;
};

struct Rig {
    Rig()
        : auth{keys, wall, {.skew_ms = 5'000}}, book{ledger, 86'400ULL * 1'000'000'000ULL}, hub{"e1", 16},
          instruments{backend, wall},
          desk{backend,
               book,
               instruments,
               wall,
               [this](std::chrono::milliseconds) { ++sleeps; },
               {.wait = std::chrono::milliseconds{100}, .poll = std::chrono::milliseconds{50}}},
          feed{backend, book, hub, instruments, wall},
          router{auth,
                 desk,
                 hub,
                 audit,
                 book,
                 wall,
                 &extension,
                 {Extension{.name = "desk",
                            .port = 8796,
                            .permission = Permission::Trade,
                            .routes = {{.method = "POST", .path = "/requests"},
                                       {.method = "GET", .path = "/requests"},
                                       {.method = "GET", .path = "/requests/*"},
                                       {.method = "POST", .path = "/requests/*/accept"}}}}} {
        backend.listings["1"] = test::listing("1", "binance", "BTC/USDT");
        backend.listings["2"] = test::listing("2", "okx", "ETH/USDT");
        keys.add(full_key());
    }

    struct Sent {
        EdgeResponse response;

        std::string header(std::string_view name) const {
            for (const auto& [key, value] : response.headers) {
                if (key == name) {
                    return value;
                }
            }
            return {};
        }
    };

    Sent call(std::string_view method,
              std::string_view path,
              std::string_view body = {},
              std::string_view key = "ak1",
              std::string idem = "",
              std::string_view query = {}) {
        const auto ts = std::to_string(now_ns.load() / 1'000'000ULL + (++ms_salt % 1000));
        const auto signature = sign(canonical_request(ts, method, path, query, body));
        if (idem.empty() && method != "GET") {
            idem = "idem-key-" + std::to_string(++idem_counter);
        }
        const EdgeRequest request{.method = method,
                                  .path = path,
                                  .query = query,
                                  .body = body,
                                  .key_id = key,
                                  .timestamp = ts,
                                  .signature = signature,
                                  .idempotency_key = idem,
                                  .peer_ip = "203.0.113.5"};
        return {router.handle(request)};
    }

    std::string place(std::string_view key = "ak1", std::string idem = "") {
        const auto sent = call(
            "POST",
            "/api/v1/orders",
            R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})",
            key,
            std::move(idem));
        EXPECT_EQ(sent.response.status, 202) << sent.response.body;
        return std::to_string(backend.next_order);
    }

    std::atomic<std::uint64_t> now_ns{k_start_ns};
    WallClock wall = [this] { return UnixNanos{now_ns.load()}; };
    FakeKeys keys;
    FakeBackend backend;
    MemoryLedger ledger;
    MemoryAudit audit;
    FakeExtension extension;
    Authenticator auth;
    ClientBook book;
    StreamHub hub;
    InstrumentDirectory instruments;
    int sleeps{0};
    ClientDesk desk;
    PrivateFeed feed;
    ApiRouter router;
    std::uint64_t idem_counter{0};
    std::uint64_t ms_salt{0};
};

bool has_code(const EdgeResponse& response, std::string_view code) {
    return response.body.find("\"code\":\"" + std::string{code} + "\"") != std::string::npos;
}

TEST(Signing, ValidRequestPasses) {
    Rig rig;
    const auto sent = rig.call("GET", "/api/v1/whoami");
    EXPECT_EQ(sent.response.status, 200);
    EXPECT_NE(sent.response.body.find("\"client_id\":\"acme\""), std::string::npos);
    EXPECT_FALSE(sent.header("X-Request-Id").empty());
    EXPECT_FALSE(sent.header("RateLimit-Limit").empty());
}

TEST(Signing, ATamperedBodyQueryOrMethodIsRefused) {
    Rig rig;
    const auto ts = std::to_string(k_start_ns / 1'000'000ULL);
    const auto signature = sign(canonical_request(ts, "POST", "/api/v1/orders", "", "{}"));
    EdgeRequest request{.method = "POST",
                        .path = "/api/v1/orders",
                        .body = R"({"qty":"9"})",
                        .key_id = "ak1",
                        .timestamp = ts,
                        .signature = signature,
                        .idempotency_key = "abcdefgh1"};
    EXPECT_TRUE(has_code(rig.router.handle(request), "auth-failed"));
    request.body = "{}";
    request.query = "x=1";
    EXPECT_TRUE(has_code(rig.router.handle(request), "auth-failed"));
    request.query = {};
    request.method = "DELETE";
    EXPECT_TRUE(has_code(rig.router.handle(request), "auth-failed"));
    EXPECT_EQ(rig.backend.placed.size(), 0U);
}

TEST(Signing, UnknownKeyAndBadSignatureAnswerTheSame) {
    Rig rig;
    const auto ts = std::to_string(k_start_ns / 1'000'000ULL);
    const auto stranger_signature = sign(canonical_request(ts, "GET", "/api/v1/whoami", "", ""));
    const EdgeRequest stranger{
        .method = "GET", .path = "/api/v1/whoami", .key_id = "who", .timestamp = ts, .signature = stranger_signature};
    const std::string fake_signature(64, 'a');
    const EdgeRequest forged{
        .method = "GET", .path = "/api/v1/whoami", .key_id = "ak1", .timestamp = ts, .signature = fake_signature};
    const auto a = rig.router.handle(stranger);
    const auto b = rig.router.handle(forged);
    EXPECT_EQ(a.status, 401);
    EXPECT_EQ(b.status, 401);
    EXPECT_TRUE(has_code(a, "auth-failed"));
    EXPECT_TRUE(has_code(b, "auth-failed"));
    EXPECT_GE(rig.audit.count("auth.refused"), 2U);
}

TEST(Signing, TimestampOutsideTheWindowIsRefusedBothWays) {
    Rig rig;
    const auto check = [&rig](std::uint64_t ms) {
        const auto ts = std::to_string(ms);
        const auto signature = sign(canonical_request(ts, "GET", "/api/v1/whoami", "", ""));
        return rig.router.handle(
            {.method = "GET", .path = "/api/v1/whoami", .key_id = "ak1", .timestamp = ts, .signature = signature});
    };
    const auto now_ms = k_start_ns / 1'000'000ULL;
    EXPECT_EQ(check(now_ms + 4'000).status, 200);
    EXPECT_EQ(check(now_ms - 4'000).status, 200);
    EXPECT_TRUE(has_code(check(now_ms + 6'000), "timestamp-skew"));
    EXPECT_TRUE(has_code(check(now_ms - 6'000), "timestamp-skew"));
}

TEST(Signing, AReplayedSignatureIsRefused) {
    Rig rig;
    const auto ts = std::to_string(k_start_ns / 1'000'000ULL);
    const auto request_signature = sign(canonical_request(ts, "GET", "/api/v1/whoami", "", ""));
    const EdgeRequest request{
        .method = "GET", .path = "/api/v1/whoami", .key_id = "ak1", .timestamp = ts, .signature = request_signature};
    EXPECT_EQ(rig.router.handle(request).status, 200);
    const auto again = rig.router.handle(request);
    EXPECT_EQ(again.status, 401);
    EXPECT_TRUE(has_code(again, "replayed"));
}

TEST(Signing, RepeatedBadAuthFromOneAddressIsThrottled) {
    Rig rig;
    const auto ts = std::to_string(k_start_ns / 1'000'000ULL);
    const std::string fake_signature(64, 'a');
    const EdgeRequest forged{.method = "GET",
                             .path = "/api/v1/whoami",
                             .key_id = "ak1",
                             .timestamp = ts,
                             .signature = fake_signature,
                             .peer_ip = "198.51.100.9"};
    int throttled = 0;
    for (int i = 0; i < 40; ++i) {
        throttled += rig.router.handle(forged).status == 429 ? 1 : 0;
    }
    EXPECT_GE(throttled, 9);
}

TEST(Access, PublicRoutesNeedNoKey) {
    Rig rig;
    EXPECT_EQ(rig.router.handle({.method = "GET", .path = "/api/v1/time"}).status, 200);
    EXPECT_EQ(rig.router.handle({.method = "GET", .path = "/api/v1/health"}).status, 200);
    EXPECT_EQ(rig.router.handle({.method = "GET", .path = "/api/v1/orders"}).status, 401);
    EXPECT_EQ(rig.router.handle({.method = "GET", .path = "/v1/orders"}).status, 404);
}

TEST(Access, PermissionsGateEachRoute) {
    Rig rig;
    auto read_only = full_key("reader");
    read_only.permissions = static_cast<std::uint8_t>(Permission::Read);
    rig.keys.add(read_only);
    auto trader = full_key("trader");
    trader.permissions = static_cast<std::uint8_t>(Permission::Trade);
    rig.keys.add(trader);

    const auto order =
        R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/orders", order, "reader").response, "permission-denied"));
    EXPECT_EQ(rig.backend.placed.size(), 0U);
    EXPECT_EQ(rig.call("POST", "/api/v1/orders", order, "trader").response.status, 202);
    EXPECT_TRUE(has_code(rig.call("GET", "/api/v1/orders", {}, "trader").response, "permission-denied"));
    const auto algo = R"({"symbol":"BTC/USDT","side":"buy","qty":"1","algo":"twap"})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/algos", algo, "trader").response, "permission-denied"));
    EXPECT_GE(rig.audit.count("algo.start"), 1U);
}

TEST(Access, TheIpAllowlistHolds) {
    Rig rig;
    auto pinned = full_key("pinned");
    pinned.ips.push_back(*parse_ip_range("10.0.0.0/8"));
    rig.keys.add(pinned);
    EXPECT_TRUE(has_code(rig.call("GET", "/api/v1/whoami", {}, "pinned").response, "ip-not-allowed"));
}

TEST(Limits, FourTwentyNineWithRetryAfterAndHeaders) {
    Rig rig;
    auto slow = full_key("slow");
    slow.rate = {.requests_per_second = 1, .burst = 2, .mutations_per_second = 1, .mutation_burst = 1};
    rig.keys.add(slow);
    const auto first = rig.call("GET", "/api/v1/whoami", {}, "slow");
    EXPECT_EQ(first.response.status, 200);
    EXPECT_EQ(first.header("RateLimit-Limit"), "2");
    EXPECT_EQ(first.header("RateLimit-Remaining"), "1");
    EXPECT_EQ(rig.call("GET", "/api/v1/whoami", {}, "slow").response.status, 200);
    const auto refused = rig.call("GET", "/api/v1/whoami", {}, "slow");
    EXPECT_EQ(refused.response.status, 429);
    EXPECT_EQ(refused.header("Retry-After"), "1");
    EXPECT_EQ(refused.header("RateLimit-Remaining"), "0");
    EXPECT_EQ(refused.response.content_type, "application/problem+json");
    EXPECT_EQ(rig.audit.count("rate.refused"), 1U);
    rig.now_ns += 2'000'000'000ULL;
    EXPECT_EQ(rig.call("GET", "/api/v1/whoami", {}, "slow").response.status, 200);
}

TEST(Limits, MutationsHaveTheirOwnTighterBucket) {
    Rig rig;
    auto slow = full_key("slow");
    slow.rate = {.requests_per_second = 100, .burst = 100, .mutations_per_second = 1, .mutation_burst = 1};
    rig.keys.add(slow);
    const auto order =
        R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    EXPECT_EQ(rig.call("POST", "/api/v1/orders", order, "slow").response.status, 202);
    EXPECT_EQ(rig.call("POST", "/api/v1/orders", order, "slow").response.status, 429);
    EXPECT_EQ(rig.call("GET", "/api/v1/whoami", {}, "slow").response.status, 200);
}

TEST(Scope, AccountAndSymbolAreEnforcedFromTheListingNotTheBody) {
    Rig rig;
    auto scoped = full_key("scoped");
    scoped.accounts = {"binance"};
    scoped.symbols = {"BTC/*"};
    rig.keys.add(scoped);
    // claims binance, names an okx instrument: the desk would have routed on the instrument
    const auto
        liar = R"({"account":"binance","instrument_id":"2","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/orders", liar, "scoped").response, "account-mismatch"));
    const auto
        honest = R"({"account":"okx","instrument_id":"2","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/orders", honest, "scoped").response, "out-of-scope"));
    EXPECT_EQ(rig.backend.placed.size(), 0U);
    const auto
        fine = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    EXPECT_EQ(rig.call("POST", "/api/v1/orders", fine, "scoped").response.status, 202);
    EXPECT_EQ(rig.call("GET", "/api/v1/instruments/2", {}, "scoped").response.status, 404);
    EXPECT_NE(rig.call("GET", "/api/v1/accounts", {}, "scoped").response.body.find("binance"), std::string::npos);
    EXPECT_EQ(rig.call("GET", "/api/v1/accounts", {}, "scoped").response.body.find("okx"), std::string::npos);
    EXPECT_EQ(
        rig.call("GET", "/api/v1/options/chain", {}, "scoped", "", "underlying=SOL&expiry=20261127").response.status,
        403);
    EXPECT_EQ(
        rig.call("GET", "/api/v1/options/chain", {}, "scoped", "", "underlying=BTC&expiry=20261127").response.status,
        200);
    EXPECT_EQ(rig.call("GET", "/api/v1/options/underlyings", {}, "scoped").response.status, 403);
}

TEST(Books, EachClientsBookIsOpenedOnceAndTagsItsOrders) {
    Rig rig;
    rig.keys.add(full_key("second", "acme"));
    rig.keys.add(full_key("other", "globex"));
    (void)rig.place();
    (void)rig.place("second");
    (void)rig.place("other");
    EXPECT_EQ(rig.backend.books_opened, (std::vector<std::string>{"client:acme", "client:globex"}));
    ASSERT_EQ(rig.backend.placed.size(), 3U);
    EXPECT_EQ(rig.backend.placed[0].book, "client:acme");
    EXPECT_EQ(rig.backend.placed[1].book, "client:acme");
    EXPECT_EQ(rig.backend.placed[2].book, "client:globex");
    EXPECT_EQ(rig.call("GET", "/api/v1/orders").response.status, 200);
    EXPECT_EQ(rig.backend.last_order_book, "client:acme");
}

TEST(Books, AlgosAreTaggedToo) {
    Rig rig;
    EXPECT_EQ(rig.call("POST", "/api/v1/algos", R"({"symbol":"BTC/USDT","side":"buy","qty":"1","external_ref":"mine"})")
                  .response.status,
              201);
    ASSERT_EQ(rig.backend.started.size(), 1U);
    EXPECT_EQ(rig.backend.started[0].book, "client:acme");
    EXPECT_EQ(rig.backend.started[0].external_ref, "mine");
}

TEST(Books, AClientCannotNameABook) {
    Rig rig;
    const auto order =
        R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1","book":"client:globex"})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/orders", order).response, "out-of-scope"));
    EXPECT_TRUE(rig.backend.placed.empty());
    EXPECT_TRUE(has_code(
        rig.call("POST", "/api/v1/algos", R"({"symbol":"BTC/USDT","side":"buy","qty":"1","book":"hedge"})").response,
        "out-of-scope"));
    EXPECT_TRUE(rig.backend.started.empty());
}

TEST(Books, AnExternalRefPassesThroughAndIsBounded) {
    Rig rig;
    const auto good =
        R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1","external_ref":"ticket-7"})";
    EXPECT_EQ(rig.call("POST", "/api/v1/orders", good).response.status, 202);
    EXPECT_EQ(rig.backend.placed.at(0).external_ref, "ticket-7");
    const auto text =
        R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1","external_ref":")" +
        std::string(41, 'x') + R"("})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/orders", text).response, "external-ref-too-long"));
    EXPECT_EQ(rig.backend.placed.size(), 1U);
}

TEST(Books, NoBookMeansNoOrder) {
    Rig rig;
    rig.backend.book_failure = BackendError{.status = 503, .title = "no book registry"};
    const auto
        body = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    const auto refused = rig.call("POST", "/api/v1/orders", body);
    EXPECT_EQ(refused.response.status, 503);
    EXPECT_TRUE(rig.backend.placed.empty());
    rig.backend.book_failure.reset();
    EXPECT_EQ(rig.call("POST", "/api/v1/orders", body).response.status, 202);
}

TEST(Refusals, ADeniedOrderCarriesItsReasonAsAProblem) {
    Rig rig;
    const auto expect = [&rig](std::int64_t reject_code, std::string_view code) {
        const auto id = rig.place();
        rig.backend.rows[id] = OrderRow{
            .client_order_id = id, .instrument_id = "1", .qty = "1", .status = "denied", .reject_code = reject_code};
        const auto sent = rig.call("GET", "/api/v1/orders/" + id);
        EXPECT_NE(sent.response.body.find("\"code\":\"" + std::string{code} + "\""), std::string::npos)
            << sent.response.body;
        EXPECT_NE(sent.response.body.find("urn:raikiri:problem:" + std::string{code}), std::string::npos);
    };
    expect(k_refused_duplicate, "duplicate-order");
    expect(k_refused_naked_short, "naked-short");
    expect(k_refused_short_margin_unknown, "short-margin-unknown");
    expect(k_refused_short_margin, "short-margin");
    expect(k_refused_short_greeks, "short-greeks");
    expect(k_refused_risk, "risk-refused");
    expect(k_refused_venue_down, "venue-unavailable");
    expect(k_refused_capacity, "order-store-full");
}

TEST(Refusals, AVenuesOwnCodeIsNotTakenForOne) {
    EXPECT_FALSE(order_refusal_of(10'004).has_value());
    EXPECT_FALSE(order_refusal_of(0).has_value());
}

TEST(Scope, AnotherClientsOrderLooksLikeNoOrder) {
    Rig rig;
    rig.keys.add(full_key("other", "globex"));
    const auto id = rig.place();
    rig.backend.rows[id] = OrderRow{.client_order_id = id, .instrument_id = "1", .qty = "1", .status = "accepted"};
    const auto path = "/api/v1/orders/" + id;
    EXPECT_EQ(rig.call("GET", path).response.status, 200);
    EXPECT_EQ(rig.call("GET", path, {}, "other").response.status, 404);
    EXPECT_EQ(rig.call("DELETE", path, {}, "other").response.status, 404);
    EXPECT_TRUE(rig.backend.canceled.empty());
    EXPECT_EQ(rig.call("DELETE", path).response.status, 202);
    EXPECT_EQ(rig.backend.canceled.size(), 1U);
}

TEST(Scope, ListsShowOnlyTheClientsOwnRowsAndKeepThePageWhole) {
    Rig rig;
    rig.keys.add(full_key("other", "globex"));
    const auto mine = rig.place();
    rig.backend.order_list.push_back(OrderRow{.client_order_id = "999", .status = "accepted"});
    rig.backend.order_list.push_back(OrderRow{.client_order_id = mine, .instrument_id = "1", .status = "accepted"});
    rig.backend.order_list.push_back(OrderRow{.client_order_id = "998", .status = "accepted"});
    const auto page = rig.call("GET", "/api/v1/orders", {}, "ak1", "", "limit=1");
    EXPECT_EQ(page.response.status, 200);
    EXPECT_NE(page.response.body.find(mine), std::string::npos);
    EXPECT_EQ(page.response.body.find("999"), std::string::npos);
    EXPECT_NE(page.response.body.find("\"client_id\":\"acme\""), std::string::npos);
    EXPECT_NE(page.response.body.find("next_cursor"), std::string::npos);
    const auto theirs = rig.call("GET", "/api/v1/orders", {}, "other", "", "limit=50");
    EXPECT_EQ(theirs.response.body.find(mine), std::string::npos);
}

TEST(Idempotency, EveryMutationNeedsAKey) {
    Rig rig;
    const auto ts = std::to_string(k_start_ns / 1'000'000ULL);
    const auto
        body = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    const auto request_signature = sign(canonical_request(ts, "POST", "/api/v1/orders", "", body));
    const EdgeRequest request{.method = "POST",
                              .path = "/api/v1/orders",
                              .body = body,
                              .key_id = "ak1",
                              .timestamp = ts,
                              .signature = request_signature};
    EXPECT_TRUE(has_code(rig.router.handle(request), "idempotency-key-required"));
    EXPECT_TRUE(rig.backend.placed.empty());
}

TEST(Idempotency, ARepeatAnswersTheFirstOrderAndPlacesNothing) {
    Rig rig;
    const auto
        body = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    const auto first = rig.call("POST", "/api/v1/orders", body, "ak1", "retry-me-1");
    const auto second = rig.call("POST", "/api/v1/orders", body, "ak1", "retry-me-1");
    EXPECT_EQ(first.response.status, 202);
    EXPECT_EQ(second.response.status, 202);
    EXPECT_EQ(second.response.body, first.response.body);
    EXPECT_EQ(second.header("Idempotent-Replay"), "true");
    EXPECT_EQ(rig.backend.placed.size(), 1U);
    const auto other_body =
        R"({"account":"binance","instrument_id":"1","side":"sell","order_type":"limit","price":"100","qty":"1"})";
    EXPECT_TRUE(
        has_code(rig.call("POST", "/api/v1/orders", other_body, "ak1", "retry-me-1").response, "idempotency-mismatch"));
}

TEST(Idempotency, TwoClientsCanUseTheSameKey) {
    Rig rig;
    rig.keys.add(full_key("other", "globex"));
    (void)rig.place("ak1", "shared-key-1");
    (void)rig.place("other", "shared-key-1");
    EXPECT_EQ(rig.backend.placed.size(), 2U);
    EXPECT_NE(rig.backend.keys_seen[0], rig.backend.keys_seen[1]);
}

TEST(Idempotency, ARefusalLeavesTheKeyFreeToRetry) {
    Rig rig;
    rig.backend.place_failure = BackendError{.status = 503, .title = "ring full"};
    const auto
        body = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    const auto refused = rig.call("POST", "/api/v1/orders", body, "ak1", "retry-me-2");
    EXPECT_EQ(refused.response.status, 503);
    EXPECT_EQ(refused.header("Retry-After"), "1");
    rig.backend.place_failure.reset();
    EXPECT_EQ(rig.call("POST", "/api/v1/orders", body, "ak1", "retry-me-2").response.status, 202);
}

TEST(Idempotency, AnUnansweredCallIsNeverSentTwice) {
    Rig rig;
    rig.backend.place_failure = BackendError{.failure = BackendFailure::NoAnswer};
    const auto
        body = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    const auto lost = rig.call("POST", "/api/v1/orders", body, "ak1", "retry-me-3");
    EXPECT_EQ(lost.response.status, 504);
    EXPECT_TRUE(has_code(lost.response, "backend-timeout"));
    rig.backend.place_failure.reset();
    const auto again = rig.call("POST", "/api/v1/orders", body, "ak1", "retry-me-3");
    EXPECT_TRUE(has_code(again.response, "outcome-unknown"));
    EXPECT_EQ(rig.backend.placed.size(), 1U);
}

TEST(Idempotency, NotDurableMeansNotSent) {
    Rig rig;
    rig.ledger.fail = true;
    const auto
        body = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/orders", body).response, "ledger-unavailable"));
    EXPECT_TRUE(rig.backend.placed.empty());
}

TEST(Idempotency, SurvivesARestartThroughTheLedger) {
    Rig rig;
    const auto
        body = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";
    const auto first = rig.call("POST", "/api/v1/orders", body, "ak1", "retry-me-4");
    ClientBook reborn{rig.ledger, 86'400ULL * 1'000'000'000ULL};
    reborn.load(k_start_ns);
    bool durable = true;
    const auto scoped = std::string{"acme\nretry-me-4"};
    // the hash is what the router computed; a mismatch here would mean the record was lost
    ASSERT_TRUE(rig.ledger.records.size() >= 2);
    const auto hash = rig.ledger.records.front().idem->request_hash;
    const auto begun = reborn.idem_begin(scoped, hash, k_start_ns + 1, durable);
    EXPECT_EQ(begun.verdict, IdemVerdict::Replay);
    EXPECT_EQ(begun.reply.body, first.response.body);
    EXPECT_TRUE(reborn.find_order(std::to_string(rig.backend.next_order)).has_value());
}

TEST(Audit, MutationsAndRefusalsAreWrittenWithoutSecrets) {
    Rig rig;
    const auto id = rig.place();
    EXPECT_EQ(rig.call("DELETE", "/api/v1/orders/" + id).response.status, 202);
    EXPECT_EQ(rig.audit.count("order.place"), 1U);
    EXPECT_EQ(rig.audit.count("order.cancel"), 1U);
    EXPECT_EQ(rig.call("DELETE", "/api/v1/orders/424242").response.status, 404);
    EXPECT_EQ(rig.audit.count("order.cancel"), 2U);
    for (const auto& line : rig.audit.lines) {
        EXPECT_EQ(line.detail.find(test::k_secret), std::string::npos);
    }
    EXPECT_NE(rig.audit.lines.front().detail.find("request_id"), std::string::npos);
}

TEST(Amend, CancelsConfirmsThenPlacesWhatRemains) {
    Rig rig;
    const auto id = rig.place();
    rig.backend.rows[id] = OrderRow{
        .client_order_id = id, .instrument_id = "1", .side = "buy", .qty = "1", .leaves = "0.75", .status = "accepted"};
    const auto sent = rig.call("POST", "/api/v1/orders/" + id + "/amend", R"({"price":"101","qty":"1"})");
    EXPECT_EQ(sent.response.status, 202) << sent.response.body;
    EXPECT_NE(sent.response.body.find("\"replaces\":\"" + id + "\""), std::string::npos);
    ASSERT_EQ(rig.backend.placed.size(), 2U);
    EXPECT_EQ(rig.backend.placed[1].price, "101");
    EXPECT_EQ(rig.backend.placed[1].qty, "0.75");
    EXPECT_EQ(rig.backend.canceled.front(), id);
}

TEST(Amend, TheLiveFeedConfirmsWithoutTheDatabase) {
    Rig rig;
    const auto id = rig.place();
    rig.feed.on_order_event(
        {.client_order_id = id, .instrument_id = "1", .event_type = "filled", .leaves = "0.75", .ts_event = "5"});
    rig.feed.on_order_event(
        {.client_order_id = id, .instrument_id = "1", .event_type = "canceled", .leaves = "0", .ts_event = "6"});
    // no row at all: as if admind had no postgres, or the persister were behind
    const auto sent = rig.call("POST", "/api/v1/orders/" + id + "/amend", R"({"price":"101"})");
    EXPECT_EQ(sent.response.status, 202) << sent.response.body;
    ASSERT_EQ(rig.backend.placed.size(), 2U);
    EXPECT_EQ(rig.backend.placed[1].qty, "0.75");
}

TEST(Amend, AFillSeenOnTheFeedBeforeTheCloseMeansNoReplacement) {
    Rig rig;
    const auto id = rig.place();
    rig.feed.on_order_event(
        {.client_order_id = id, .instrument_id = "1", .event_type = "filled", .leaves = "0", .ts_event = "5"});
    const auto sent = rig.call("POST", "/api/v1/orders/" + id + "/amend", R"({"price":"101"})");
    EXPECT_TRUE(has_code(sent.response, "order-closed"));
    EXPECT_EQ(rig.backend.placed.size(), 1U);
}

TEST(Amend, NothingIsPlacedUntilTheCancelIsConfirmed) {
    Rig rig;
    rig.backend.close_on_cancel = false;
    const auto id = rig.place();
    rig.backend.rows[id] = OrderRow{.client_order_id = id, .qty = "1", .status = "accepted"};
    const auto sent = rig.call("POST", "/api/v1/orders/" + id + "/amend", R"({"price":"101"})");
    EXPECT_EQ(sent.response.status, 409);
    EXPECT_TRUE(has_code(sent.response, "amend-unconfirmed"));
    EXPECT_EQ(rig.backend.placed.size(), 1U);
    EXPECT_GE(rig.sleeps, 1);
}

TEST(Amend, AFilledOrderIsNotReplaced) {
    Rig rig;
    rig.backend.close_on_cancel = false;
    const auto id = rig.place();
    rig.backend.rows[id] = OrderRow{.client_order_id = id, .qty = "1", .leaves = "0", .status = "filled"};
    const auto sent = rig.call("POST", "/api/v1/orders/" + id + "/amend", R"({"price":"101"})");
    EXPECT_TRUE(has_code(sent.response, "order-closed"));
    EXPECT_EQ(rig.backend.placed.size(), 1U);
}

TEST(Amend, ShrinkingBelowWhatFilledPlacesNothing) {
    Rig rig;
    const auto id = rig.place();
    rig.backend.rows[id] = OrderRow{.client_order_id = id, .qty = "1", .leaves = "0.25", .status = "accepted"};
    const auto sent = rig.call("POST", "/api/v1/orders/" + id + "/amend", R"({"qty":"0.5"})");
    EXPECT_EQ(sent.response.status, 202);
    EXPECT_NE(sent.response.body.find("\"status\":\"canceled\""), std::string::npos);
    EXPECT_EQ(rig.backend.placed.size(), 1U);
}

TEST(Bodies, AMisspeltFieldIsRefusedNotIgnored) {
    Rig rig;
    const auto typo =
        R"({"account":"binance","instrument_id":"1","side":"sell","order_type":"limit","price":"100","qty":"1","reduceonly":true})";
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/orders", typo).response, "bad-request"));
    EXPECT_TRUE(rig.backend.placed.empty());
}

TEST(Paths, AnIdThatIsNotDigitsDoesNotReachTheBackend) {
    Rig rig;
    EXPECT_EQ(rig.call("GET", "/api/v1/instruments/1%2F..%2F..").response.status, 404);
    EXPECT_EQ(rig.call("GET", "/api/v1/instruments/abc").response.status, 404);
    EXPECT_EQ(rig.call("PUT", "/api/v1/orders").response.status, 405);
    EXPECT_EQ(rig.call("GET", "/api/v1/nothing").response.status, 404);
}

TEST(Algos, ScopedToTheClientAndItsSymbols) {
    Rig rig;
    rig.keys.add(full_key("other", "globex"));
    const auto twap = R"({"symbol":"BTC/USDT","side":"buy","qty":"1","algo":"twap","horizon_s":60,"slices":6})";
    const auto started = rig.call("POST", "/api/v1/algos", twap);
    EXPECT_EQ(started.response.status, 201) << started.response.body;
    EXPECT_NE(started.response.body.find("\"client_id\":\"acme\""), std::string::npos);
    EXPECT_EQ(rig.call("GET", "/api/v1/algos/1").response.status, 200);
    EXPECT_EQ(rig.call("GET", "/api/v1/algos/1", {}, "other").response.status, 404);
    EXPECT_EQ(rig.call("POST", "/api/v1/algos/1/pause", {}, "other").response.status, 404);
    EXPECT_TRUE(rig.backend.actions.empty());
    EXPECT_EQ(rig.call("POST", "/api/v1/algos/1/pause").response.status, 200);
    EXPECT_EQ(rig.backend.actions.front(), "1:pause");
    EXPECT_EQ(rig.call("GET", "/api/v1/algos", {}, "other").response.body.find("\"id\":\"1\""), std::string::npos);

    auto limited = full_key("limited");
    limited.accounts = {"binance"};
    rig.keys.add(limited);
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/algos", twap, "limited").response, "out-of-scope"));
    auto symbols = full_key("symbols");
    symbols.symbols = {"ETH/*"};
    rig.keys.add(symbols);
    EXPECT_TRUE(has_code(rig.call("POST", "/api/v1/algos", twap, "symbols").response, "out-of-scope"));
}

TEST(Algos, AReusedParentIdIsNotTheClientsParent) {
    Rig rig;
    const auto twap = R"({"symbol":"BTC/USDT","side":"buy","qty":"1","algo":"twap"})";
    EXPECT_EQ(rig.call("POST", "/api/v1/algos", twap).response.status, 201);
    // strategyd restarted and another client's parent now holds id 1
    rig.backend.parents["1"].created = "2000";
    const auto seen = rig.call("GET", "/api/v1/algos/1");
    EXPECT_EQ(seen.response.status, 410);
    EXPECT_TRUE(has_code(seen.response, "algo-lost"));
    EXPECT_EQ(rig.call("POST", "/api/v1/algos/1/cancel").response.status, 410);
    EXPECT_TRUE(rig.backend.actions.empty());
}

TEST(Feed, AnInternalFillIsMarkedAndTellsTheOrderItsState) {
    Rig rig;
    const auto id = rig.place();
    const auto before = rig.hub.head("acme");
    rig.feed.on_fill({.venue = "binance",
                      .exec_id = "ixab",
                      .client_order_id = id,
                      .instrument_id = "1",
                      .side = "buy",
                      .price = "100",
                      .qty = "1",
                      .liquidity = "internal",
                      .ts_event = "6"});
    // the fill, the position, and the order state no gateway would have sent
    EXPECT_EQ(rig.hub.head("acme"), before + 3);
    const auto fills = rig.call("GET", "/api/v1/fills");
    EXPECT_NE(fills.response.body.find("\"internal\":true"), std::string::npos);
    EXPECT_NE(fills.response.body.find("\"liquidity\":\"internal\""), std::string::npos);
    rig.feed.on_fill({.venue = "binance",
                      .exec_id = "v1",
                      .client_order_id = id,
                      .instrument_id = "1",
                      .side = "buy",
                      .price = "100",
                      .qty = "1",
                      .liquidity = "taker",
                      .ts_event = "7"});
    const auto both = rig.call("GET", "/api/v1/fills");
    const auto venue = both.response.body.find("\"exec_id\":\"v1\"");
    ASSERT_NE(venue, std::string::npos);
    EXPECT_EQ(both.response.body.substr(venue, 200).find("\"internal\":true"), std::string::npos);
}

TEST(Feed, RoutesEventsToTheirOwnerAndFoldsPositions) {
    Rig rig;
    rig.keys.add(full_key("other", "globex"));
    const auto id = rig.place();
    rig.feed.on_order_event({.client_order_id = id, .instrument_id = "1", .event_type = "accepted", .ts_event = "5"});
    rig.feed.on_fill({.venue = "binance",
                      .exec_id = "x1",
                      .client_order_id = id,
                      .instrument_id = "1",
                      .side = "buy",
                      .price = "100",
                      .qty = "1",
                      .fee = "0.1",
                      .ts_event = "6"});
    rig.feed.on_fill({.venue = "binance",
                      .exec_id = "x1",
                      .client_order_id = id,
                      .instrument_id = "1",
                      .side = "buy",
                      .price = "100",
                      .qty = "1",
                      .ts_event = "6"});
    EXPECT_EQ(rig.hub.head("acme"), 3U);
    EXPECT_EQ(rig.hub.head("globex"), 0U);
    const auto fills = rig.call("GET", "/api/v1/fills");
    EXPECT_NE(fills.response.body.find("\"exec_id\":\"x1\""), std::string::npos);
    EXPECT_NE(fills.response.body.find("\"client_id\":\"acme\""), std::string::npos);
    EXPECT_EQ(rig.call("GET", "/api/v1/fills", {}, "other").response.body.find("x1"), std::string::npos);
    const auto positions = rig.call("GET", "/api/v1/positions");
    EXPECT_NE(positions.response.body.find("\"qty\":\"1"), std::string::npos) << positions.response.body;
    EXPECT_NE(positions.response.body.find("\"avg_entry_price\":\"100"), std::string::npos) << positions.response.body;
    EXPECT_EQ(rig.call("GET", "/api/v1/positions", {}, "other").response.body.find("BTC/USDT"), std::string::npos);
}

TEST(Feed, AnEventThatOutrunsItsOrderIsHeldNotLost) {
    Rig rig;
    OwnedOrder later{.id = "77",
                     .client_id = "acme",
                     .key_id = "ak1",
                     .account = "binance",
                     .symbol = "BTC/USDT",
                     .instrument_id = "1",
                     .side = "buy"};
    rig.feed.on_fill({.venue = "binance",
                      .exec_id = "x9",
                      .client_order_id = "77",
                      .instrument_id = "1",
                      .side = "buy",
                      .price = "100",
                      .qty = "1",
                      .ts_event = "6"});
    EXPECT_EQ(rig.hub.head("acme"), 0U);
    ASSERT_TRUE(rig.book.record_order(later));
    (void)rig.feed.tick();
    EXPECT_EQ(rig.hub.head("acme"), 2U);
}

TEST(Feed, ChildrenOfAnAlgoAreAttributedFromTheParent) {
    Rig rig;
    const auto twap = R"({"symbol":"BTC/USDT","side":"buy","qty":"1","algo":"twap"})";
    EXPECT_EQ(rig.call("POST", "/api/v1/algos", twap).response.status, 201);
    rig.backend.parents["1"].children = std::vector<ChildRow>{
        {.client_order_id = "500", .instrument_id = "1", .qty = "0.1", .limit = "100"}};
    rig.feed.on_fill({.venue = "binance",
                      .exec_id = "c1",
                      .client_order_id = "500",
                      .instrument_id = "1",
                      .side = "buy",
                      .price = "100",
                      .qty = "0.1",
                      .ts_event = "6"});
    (void)rig.feed.tick();
    const auto order = rig.book.find_order("500");
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->client_id, "acme");
    EXPECT_EQ(order->parent_id, "1");
    EXPECT_NE(rig.call("GET", "/api/v1/fills").response.body.find("\"exec_id\":\"c1\""), std::string::npos);
    EXPECT_EQ(rig.call("DELETE", "/api/v1/orders/500").response.status, 409);
}

TEST(Feed, ALostParentIsReportedOnceAndClosed) {
    Rig rig;
    EXPECT_EQ(rig.call("POST", "/api/v1/algos", R"({"symbol":"BTC/USDT","side":"buy","qty":"1"})").response.status,
              201);
    rig.backend.parents.clear();
    const auto before = rig.hub.head("acme");
    (void)rig.feed.tick();
    EXPECT_EQ(rig.hub.head("acme"), before + 1);
    EXPECT_TRUE(rig.book.open_parents().empty());
}

TEST(Feed, FillsMissedByTheStreamComeBackFromThePull) {
    Rig rig;
    const auto id = rig.place();
    rig.backend.fill_page.items.push_back({.venue = "binance",
                                           .exec_id = "late",
                                           .client_order_id = id,
                                           .instrument_id = "1",
                                           .side = "buy",
                                           .price = "100",
                                           .qty = "1",
                                           .ts_event = "6"});
    rig.backend.fill_page.items.push_back({.venue = "binance",
                                           .exec_id = "theirs",
                                           .client_order_id = "1",
                                           .instrument_id = "1",
                                           .side = "buy",
                                           .price = "100",
                                           .qty = "1",
                                           .ts_event = "6"});
    (void)rig.feed.tick();
    const auto fills = rig.call("GET", "/api/v1/fills");
    EXPECT_NE(fills.response.body.find("late"), std::string::npos);
    EXPECT_EQ(fills.response.body.find("theirs"), std::string::npos);
}

TEST(Stream, TheUpgradeIsSignedAndStreamsAreCapped) {
    Rig rig;
    auto capped = full_key("capped");
    capped.max_streams = 1;
    rig.keys.add(capped);
    const auto open = [&rig](std::uint64_t ms) {
        const auto ts = std::to_string(ms);
        return rig.router.authorize_stream({.method = "GET",
                                            .path = k_stream_path,
                                            .key_id = "capped",
                                            .timestamp = ts,
                                            .signature = sign(canonical_request(ts, "GET", k_stream_path, "", ""))});
    };
    const auto now_ms = k_start_ns / 1'000'000ULL;
    EXPECT_TRUE(open(now_ms).has_value());
    const auto second = open(now_ms + 1);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().status, 429);
    rig.hub.release_stream("capped");
    EXPECT_TRUE(open(now_ms + 2).has_value());
    const auto bad = rig.router.authorize_stream({.method = "GET", .path = k_stream_path, .key_id = "capped"});
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().status, 401);
}

TEST(Stream, MarketChannelsAreScoped) {
    Rig rig;
    auto scoped = full_key("scoped");
    scoped.symbols = {"BTC/*"};
    const auto key = rig.keys.add(scoped);
    EXPECT_TRUE(rig.router.check_market(*key, {"1"}).has_value());
    EXPECT_FALSE(rig.router.check_market(*key, {"1", "2"}).has_value());
    EXPECT_FALSE(rig.router.check_market(*key, {"not-digits"}).has_value());
}

TEST(Hub, ResumeReplaysWhatIsLeftAndNamesWhatIsNot) {
    StreamHub hub{"e1", 4};
    const auto publish = [&hub](int times) {
        for (int i = 0; i < times; ++i) {
            hub.publish("acme", mask_of(Channel::Fills), "binance", "BTC/USDT", FeedGap{});
        }
    };
    publish(3);
    const auto all = hub.since("acme", "e1", 1);
    EXPECT_FALSE(all.gap);
    ASSERT_EQ(all.events.size(), 2U);
    EXPECT_EQ(all.events.front()->seq, 2U);
    publish(5);
    const auto late = hub.since("acme", "e1", 1);
    EXPECT_TRUE(late.gap);
    EXPECT_EQ(late.gap_from, 2U);
    EXPECT_EQ(late.gap_to, 4U);
    EXPECT_EQ(late.events.size(), 4U);
    const auto reborn = hub.since("acme", "e0", 7);
    EXPECT_TRUE(reborn.gap);
    EXPECT_TRUE(reborn.events.empty());
    EXPECT_TRUE(hub.since("acme", "e1", 8).events.empty());
    EXPECT_FALSE(hub.since("acme", "e1", 8).gap);
}

TEST(Hub, AKeySeesOnlyItsScopeOfTheClientsRing) {
    ApiKey narrow{.accounts = {"binance"}, .symbols = {"BTC/*"}};
    EXPECT_TRUE(visible_to(narrow, {.account = "binance", .symbol = "BTC/USDT"}));
    EXPECT_FALSE(visible_to(narrow, {.account = "okx", .symbol = "BTC/USDT"}));
    EXPECT_FALSE(visible_to(narrow, {.account = "binance", .symbol = "ETH/USDT"}));
    EXPECT_TRUE(visible_to(narrow, {.account = "binance"}));
    EXPECT_FALSE(visible_to(narrow, {.account = "okx"}));
}

TEST(Frames, ParsesWhatAClientMaySend) {
    const auto market = parse_client_op(R"({"op":"subscribe","id":"q1","quotes":["1","2"],"hz":5})");
    ASSERT_TRUE(market.has_value());
    EXPECT_EQ(market->instruments.size(), 2U);
    EXPECT_NE(market->market_message.find("\"quotes\":[\"1\",\"2\"]"), std::string::npos);
    const auto mine = parse_client_op(
        R"({"op":"subscribe","id":"p","channels":["fills","positions"],"resume_from":"12","epoch":"e1"})");
    ASSERT_TRUE(mine.has_value());
    EXPECT_EQ(mine->resume_from, 12U);
    EXPECT_EQ(mine->channels, mask_of(Channel::Fills) | mask_of(Channel::Positions));
    EXPECT_FALSE(parse_client_op(R"({"op":"subscribe","id":"p","channels":["secrets"]})").has_value());
    EXPECT_FALSE(parse_client_op(R"({"op":"subscribe","id":"p"})").has_value());
    EXPECT_FALSE(parse_client_op(R"({"op":"bomb"})").has_value());
    EXPECT_FALSE(parse_client_op("not json").has_value());
    // recon and health exist on the backend's stream and stop here
    EXPECT_FALSE(parse_client_op(R"({"op":"subscribe","id":"p","events":["recon"]})").has_value());
}

TEST(Extensions, OnlyListedRoutesAreReachable) {
    Rig rig;
    EXPECT_EQ(rig.call("GET", "/api/v1/x/desk/clients").response.status, 404);
    EXPECT_EQ(rig.call("POST", "/api/v1/x/desk/rules", "{}").response.status, 404);
    EXPECT_EQ(rig.call("GET", "/api/v1/x/none/requests").response.status, 404);
    EXPECT_TRUE(rig.extension.calls.empty());
}

TEST(Extensions, TheClientIsSetByTheKeyNotTheBody) {
    Rig rig;
    rig.extension.replies["POST /requests"] = {.status = 201, .body = R"({"id":"r1","client":"acme"})"};
    const auto sent = rig.call("POST", "/api/v1/x/desk/requests", R"({"symbol":"BTC/USDT","qty":"1"})");
    EXPECT_EQ(sent.response.status, 201) << sent.response.body;
    ASSERT_EQ(rig.extension.calls.size(), 1U);
    EXPECT_NE(rig.extension.calls[0].find("\"client\":\"acme\""), std::string::npos);
    EXPECT_TRUE(
        has_code(rig.call("POST", "/api/v1/x/desk/requests", R"({"client":"globex","symbol":"BTC/USDT"})").response,
                 "out-of-scope"));
    EXPECT_GE(rig.audit.count("x.desk.POST"), 2U);
}

TEST(Extensions, ListsAreFilteredAndARecordOfAnotherClientIsHidden) {
    Rig rig;
    rig.extension.replies["GET /requests"] = {
        .status = 200, .body = R"({"now":"1","items":[{"id":"a","client":"acme"},{"id":"b","client":"globex"}]})"};
    const auto list = rig.call("GET", "/api/v1/x/desk/requests");
    EXPECT_NE(list.response.body.find("\"a\""), std::string::npos);
    EXPECT_EQ(list.response.body.find("\"b\""), std::string::npos);
    rig.extension.replies["GET /requests/b"] = {.status = 200, .body = R"({"id":"b","client":"globex"})"};
    EXPECT_EQ(rig.call("GET", "/api/v1/x/desk/requests/b").response.status, 404);
    rig.extension.calls.clear();
    EXPECT_EQ(rig.call("POST", "/api/v1/x/desk/requests/b/accept", "{}").response.status, 404);
    EXPECT_EQ(rig.extension.calls.size(), 1U);
    rig.extension.replies["GET /requests/a"] = {.status = 200, .body = R"({"id":"a","client":"acme"})"};
    rig.extension.replies["POST /requests/a/accept"] = {.status = 200,
                                                        .body = R"({"id":"a","client":"acme","state":"accepted"})"};
    EXPECT_EQ(rig.call("POST", "/api/v1/x/desk/requests/a/accept", "{}").response.status, 200);
}

} // namespace
} // namespace raikiri::clientapi
