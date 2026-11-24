#include <raikiri/admin/margin.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace raikiri::admin {
namespace {

constexpr UnixNanos k_now{1'700'000'000'000'000'000ULL};
const InstrumentId k_perp = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{3});
const InstrumentId k_spot = InstrumentId::make(VenueId::Binance, MarketType::Spot, SymbolId{3});
const InstrumentId k_unlisted = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{9});

class FakeGateway final : public MarginGateway {
public:
    std::optional<MarginTarget> target(InstrumentId instrument) const override {
        if (instrument == k_perp) {
            return MarginTarget{.account = "binance", .symbol = "BTCUSDT"};
        }
        if (instrument == k_spot) {
            return MarginTarget{.account = "binance-spot", .symbol = "BTCUSDT"};
        }
        return std::nullopt;
    }

    std::expected<MarginReply, MarginFault> ask(MarginRequest request) override {
        asked.push_back(request);
        if (fault.has_value()) {
            return std::unexpected{*fault};
        }
        auto reply = answer;
        reply.instrument = request.instrument;
        return reply;
    }

    MarginReply answer{};
    std::optional<MarginFault> fault;
    std::vector<MarginRequest> asked;
};

struct Line {
    std::string actor;
    std::string action;
    std::string subject;
    std::string detail;
};

class Journal final : public audit::Sink {
public:
    void append(const audit::Entry& entry) override {
        lines.push_back({std::string{entry.actor},
                         std::string{entry.action},
                         std::string{entry.subject},
                         std::string{entry.detail_json}});
    }

    std::vector<Line> lines;
};

MarginReply moved() {
    MarginReply reply{};
    reply.before = {.leverage = 10, .max_leverage = 0, .mode = MarginMode::Cross};
    reply.after = {.leverage = 25, .max_leverage = 125, .mode = MarginMode::Isolated};
    return reply;
}

MarginReply refused(std::string_view why, std::int64_t code) {
    MarginReply reply{};
    reply.before = {.leverage = 10, .max_leverage = 0, .mode = MarginMode::Cross};
    reply.after = reply.before;
    reply.outcome = MarginOutcome::Refused;
    reply.venue_code = code;
    why.copy(reply.message, why.size());
    return reply;
}

struct Fixture {
    FakeGateway gateway;
    Journal journal;
    MarginDesk desk{gateway, &journal, [] { return k_now; }};
};

TEST(MarginDesk, AChangeIsAskedOnceAndJournalledWithBothStates) {
    Fixture f;
    f.gateway.answer = moved();
    const auto done = f.desk.set("binance", k_perp, {.leverage = 25, .mode = MarginMode::Isolated}, "vlad");
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->state.leverage, 25U);
    EXPECT_EQ(done->state.mode, MarginMode::Isolated);

    ASSERT_EQ(f.gateway.asked.size(), 1U);
    EXPECT_EQ(f.gateway.asked[0].op, MarginOp::Set);
    EXPECT_EQ(f.gateway.asked[0].leverage, 25U);
    EXPECT_EQ(f.gateway.asked[0].mode, MarginMode::Isolated);
    EXPECT_EQ(f.gateway.asked[0].ts_created, k_now);

    ASSERT_EQ(f.journal.lines.size(), 1U);
    const auto& line = f.journal.lines[0];
    EXPECT_EQ(line.actor, "vlad");
    EXPECT_EQ(line.action, "margin.set");
    EXPECT_EQ(line.subject, "binance/BTCUSDT");
    EXPECT_NE(line.detail.find(R"("before":{"leverage":10,"margin_mode":"cross"})"), std::string::npos) << line.detail;
    EXPECT_NE(line.detail.find(R"("after":{"leverage":25,"margin_mode":"isolated"})"), std::string::npos)
        << line.detail;
    EXPECT_NE(line.detail.find(R"("outcome":"done")"), std::string::npos) << line.detail;
    EXPECT_NE(line.detail.find(R"("instrument_id":")" + std::to_string(k_perp.packed)), std::string::npos);
}

TEST(MarginDesk, AVenueRefusalIsAConflictWithTheVenueReasonAndIsJournalled) {
    Fixture f;
    f.gateway.answer = refused("Margin type cannot be changed if there exists position.", -4048);
    const auto done = f.desk.set("binance", k_perp, {.mode = MarginMode::Isolated}, "vlad");
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().status, 409);
    EXPECT_NE(done.error().detail.find("exists position"), std::string::npos);
    EXPECT_NE(done.error().detail.find("-4048"), std::string::npos);
    ASSERT_EQ(f.journal.lines.size(), 1U);
    EXPECT_NE(f.journal.lines[0].detail.find(R"("outcome":"refused")"), std::string::npos);
    EXPECT_NE(f.journal.lines[0].detail.find("exists position"), std::string::npos);
}

TEST(MarginDesk, WhatCannotBeAskedIsRefusedBeforeTheGateway) {
    Fixture f;
    EXPECT_EQ(f.desk.set("binance", k_unlisted, {.leverage = 5}, "vlad").error().status, 404);
    EXPECT_EQ(f.desk.set("bybit", k_perp, {.leverage = 5}, "vlad").error().status, 404);
    EXPECT_EQ(f.desk.set("binance-spot", k_spot, {.leverage = 5}, "vlad").error().status, 422);
    EXPECT_EQ(f.desk.set("binance", k_perp, {}, "vlad").error().status, 422);
    EXPECT_EQ(f.desk.set("binance", k_perp, {.leverage = 0}, "vlad").error().status, 422);
    EXPECT_EQ(f.desk.set("binance", k_perp, {.leverage = k_max_leverage_asked + 1}, "vlad").error().status, 422);
    EXPECT_EQ(f.desk.set("binance", k_perp, {.mode = MarginMode::Unknown}, "vlad").error().status, 422);
    EXPECT_TRUE(f.gateway.asked.empty());
    EXPECT_TRUE(f.journal.lines.empty());
}

TEST(MarginDesk, WithoutAJournalNothingIsChanged) {
    FakeGateway gateway;
    MarginDesk desk{gateway, nullptr, [] { return k_now; }};
    EXPECT_EQ(desk.set("binance", k_perp, {.leverage = 5}, "vlad").error().status, 503);
    EXPECT_TRUE(gateway.asked.empty());
    // a read needs none
    gateway.answer = moved();
    EXPECT_TRUE(desk.read("binance", k_perp).has_value());
}

TEST(MarginDesk, FaultsAndOutcomesMapToTheirOwnStatuses) {
    Fixture f;
    f.gateway.fault = MarginFault::NotServed;
    EXPECT_EQ(f.desk.read("binance", k_perp).error().status, 501);
    f.gateway.fault = MarginFault::GatewayGone;
    EXPECT_EQ(f.desk.read("binance", k_perp).error().status, 503);
    f.gateway.fault = MarginFault::NoAnswer;
    EXPECT_EQ(f.desk.set("binance", k_perp, {.leverage = 5}, "vlad").error().status, 504);
    // a change that may have landed is still on the record
    ASSERT_EQ(f.journal.lines.size(), 1U);
    EXPECT_NE(f.journal.lines[0].detail.find(R"("outcome":"no_answer")"), std::string::npos);

    f.gateway.fault.reset();
    f.gateway.answer = {};
    f.gateway.answer.outcome = MarginOutcome::Unsupported;
    EXPECT_EQ(f.desk.read("binance", k_perp).error().status, 501);
    f.gateway.answer.outcome = MarginOutcome::Failed;
    f.gateway.answer.cls = ErrorClass::RateLimited;
    EXPECT_EQ(f.desk.read("binance", k_perp).error().status, 429);
    f.gateway.answer.cls = ErrorClass::Connectivity;
    EXPECT_EQ(f.desk.read("binance", k_perp).error().status, 502);
}

TEST(MarginRoutes, OtherPathsAreLeftToTheRouter) {
    Fixture f;
    const MarginRoutes routes{f.desk};
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/accounts"}).has_value());
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/accounts//leverage"}).has_value());
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/accounts/a/b/leverage"}).has_value());
}

TEST(MarginRoutes, AReadNamesTheInstrumentInTheQuery) {
    Fixture f;
    f.gateway.answer = moved();
    const MarginRoutes routes{f.desk};
    const auto missing = routes({.method = "GET", .path = "/v1/accounts/binance/leverage"});
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->status, 400);

    const std::string query = "instrument_id=" + std::to_string(k_perp.packed);
    const auto got = routes({.method = "GET", .path = "/v1/accounts/binance/leverage", .query = query});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, 200);
    EXPECT_NE(got->body.find(R"("account":"binance")"), std::string::npos) << got->body;
    EXPECT_NE(got->body.find(R"("leverage":25)"), std::string::npos) << got->body;
    EXPECT_NE(got->body.find(R"("margin_mode":"isolated")"), std::string::npos) << got->body;
    EXPECT_NE(got->body.find(R"("max_leverage":125)"), std::string::npos) << got->body;
    ASSERT_EQ(f.gateway.asked.size(), 1U);
    EXPECT_EQ(f.gateway.asked[0].op, MarginOp::Read);
    EXPECT_TRUE(f.journal.lines.empty());
}

TEST(MarginRoutes, APutIsValidatedThenAttributedToTheLogin) {
    Fixture f;
    f.gateway.answer = moved();
    const MarginRoutes routes{f.desk};
    const std::string id = std::to_string(k_perp.packed);
    const auto put = [&](const std::string& body, std::string_view login = {}) {
        return routes({.method = "PUT", .path = "/v1/accounts/binance/leverage", .body = body, .login = login});
    };
    EXPECT_EQ(put("not json")->status, 400);
    EXPECT_EQ(put(R"({"instrument_id":"abc","leverage":5})")->status, 422);
    EXPECT_EQ(put(R"({"instrument_id":")" + id + R"(","leverage":0})")->status, 422);
    EXPECT_EQ(put(R"({"instrument_id":")" + id + R"(","margin_mode":"portfolio"})")->status, 422);
    EXPECT_EQ(put(R"({"instrument_id":")" + id + R"("})")->status, 422);
    EXPECT_TRUE(f.gateway.asked.empty());

    const auto done = put(R"({"instrument_id":")" + id + R"(","leverage":25,"margin_mode":"isolated"})", "vlad");
    EXPECT_EQ(done->status, 200);
    EXPECT_NE(done->body.find(R"("leverage":25)"), std::string::npos) << done->body;
    ASSERT_EQ(f.journal.lines.size(), 1U);
    EXPECT_EQ(f.journal.lines[0].actor, "vlad");

    EXPECT_EQ(put(R"({"instrument_id":")" + id + R"(","leverage":25})")->status, 200);
    EXPECT_EQ(f.journal.lines[1].actor, "admind");

    const auto other = routes({.method = "DELETE", .path = "/v1/accounts/binance/leverage"});
    EXPECT_EQ(other->status, 405);
}

} // namespace
} // namespace raikiri::admin
