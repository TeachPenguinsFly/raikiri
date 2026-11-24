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

    std::expected<void, MarginFault> send(MarginRequest& request) override {
        if (fault.has_value() && *fault != MarginFault::NoAnswer) {
            asked.push_back(request);
            return std::unexpected{*fault};
        }
        request.id = asked.size() + 1;
        asked.push_back(request);
        return {};
    }

    std::expected<std::optional<MarginReply>, MarginFault> collect(std::uint64_t id) override {
        if (fault == MarginFault::NoAnswer) {
            return std::unexpected{*fault};
        }
        if (hold) {
            return std::optional<MarginReply>{};
        }
        auto reply = answer;
        reply.id = id;
        reply.instrument = asked.at(id - 1).instrument;
        return std::optional<MarginReply>{reply};
    }

    MarginReply answer{};
    std::optional<MarginFault> fault;
    bool hold{false}; // replies have not landed yet
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
    reply.scope = MarginScope::OneInstrument;
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

// what a caller sees once the loop has pumped
std::expected<MarginView, MarginError> settled(MarginDesk& desk, auto begin) {
    std::optional<std::expected<MarginView, MarginError>> out;
    begin([&](std::expected<MarginView, MarginError> result) { out = std::move(result); });
    desk.pump();
    EXPECT_TRUE(out.has_value());
    return out.value_or(std::unexpected{MarginError{}});
}

std::expected<MarginView, MarginError> read_now(MarginDesk& desk, std::string_view account, InstrumentId instrument) {
    return settled(desk, [&](MarginDone done) { desk.read(account, instrument, std::move(done)); });
}

std::expected<MarginView, MarginError> set_now(
    MarginDesk& desk, std::string_view account, InstrumentId instrument, MarginChange change, std::string_view actor) {
    return settled(desk, [&](MarginDone done) { desk.set(account, instrument, change, actor, std::move(done)); });
}

// what the route answered, once it has
struct Answered {
    std::optional<http::Response> response;

    MarginRespond respond() {
        return [this](http::Response r) { response = std::move(r); };
    }
};

std::optional<http::Response> route_now(const MarginRoutes& routes, MarginDesk& desk, const http::Request& req) {
    Answered answered;
    if (!routes(req, answered.respond())) {
        return std::nullopt;
    }
    desk.pump();
    EXPECT_TRUE(answered.response.has_value());
    return answered.response;
}

struct Fixture {
    FakeGateway gateway;
    Journal journal;
    MarginDesk desk{gateway, &journal, [] { return k_now; }};
};

TEST(MarginDesk, AChangeIsAskedOnceAndJournalledWithBothStates) {
    Fixture f;
    f.gateway.answer = moved();
    const auto done = set_now(f.desk, "binance", k_perp, {.leverage = 25, .mode = MarginMode::Isolated}, "vlad");
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
    const auto done = set_now(f.desk, "binance", k_perp, {.mode = MarginMode::Isolated}, "vlad");
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
    EXPECT_EQ(set_now(f.desk, "binance", k_unlisted, {.leverage = 5}, "vlad").error().status, 404);
    EXPECT_EQ(set_now(f.desk, "bybit", k_perp, {.leverage = 5}, "vlad").error().status, 404);
    EXPECT_EQ(set_now(f.desk, "binance-spot", k_spot, {.leverage = 5}, "vlad").error().status, 422);
    EXPECT_EQ(set_now(f.desk, "binance", k_perp, {}, "vlad").error().status, 422);
    EXPECT_EQ(set_now(f.desk, "binance", k_perp, {.leverage = 0}, "vlad").error().status, 422);
    EXPECT_EQ(set_now(f.desk, "binance", k_perp, {.leverage = k_max_leverage_asked + 1}, "vlad").error().status, 422);
    EXPECT_EQ(set_now(f.desk, "binance", k_perp, {.mode = MarginMode::Unknown}, "vlad").error().status, 422);
    EXPECT_TRUE(f.gateway.asked.empty());
    EXPECT_TRUE(f.journal.lines.empty());
}

TEST(MarginDesk, WithoutAJournalNothingIsChanged) {
    FakeGateway gateway;
    MarginDesk desk{gateway, nullptr, [] { return k_now; }};
    EXPECT_EQ(set_now(desk, "binance", k_perp, {.leverage = 5}, "vlad").error().status, 503);
    EXPECT_TRUE(gateway.asked.empty());
    // a read needs none
    gateway.answer = moved();
    EXPECT_TRUE(read_now(desk, "binance", k_perp).has_value());
}

TEST(MarginDesk, FaultsAndOutcomesMapToTheirOwnStatuses) {
    Fixture f;
    f.gateway.fault = MarginFault::NotServed;
    EXPECT_EQ(read_now(f.desk, "binance", k_perp).error().status, 501);
    f.gateway.fault = MarginFault::GatewayGone;
    EXPECT_EQ(read_now(f.desk, "binance", k_perp).error().status, 503);
    f.gateway.fault = MarginFault::NoAnswer;
    EXPECT_EQ(set_now(f.desk, "binance", k_perp, {.leverage = 5}, "vlad").error().status, 504);
    // a change that may have landed is still on the record
    ASSERT_EQ(f.journal.lines.size(), 1U);
    EXPECT_NE(f.journal.lines[0].detail.find(R"("outcome":"no_answer")"), std::string::npos);

    f.gateway.fault.reset();
    f.gateway.answer = {};
    f.gateway.answer.outcome = MarginOutcome::Unsupported;
    EXPECT_EQ(read_now(f.desk, "binance", k_perp).error().status, 501);
    f.gateway.answer.outcome = MarginOutcome::Failed;
    f.gateway.answer.cls = ErrorClass::RateLimited;
    EXPECT_EQ(read_now(f.desk, "binance", k_perp).error().status, 429);
    f.gateway.answer.cls = ErrorClass::Connectivity;
    EXPECT_EQ(read_now(f.desk, "binance", k_perp).error().status, 502);
}

TEST(MarginRoutes, OtherPathsAreLeftToTheRouter) {
    Fixture f;
    const MarginRoutes routes{f.desk};
    EXPECT_FALSE(route_now(routes, f.desk, {.method = "GET", .path = "/v1/accounts"}).has_value());
    EXPECT_FALSE(route_now(routes, f.desk, {.method = "GET", .path = "/v1/accounts//leverage"}).has_value());
    EXPECT_FALSE(route_now(routes, f.desk, {.method = "GET", .path = "/v1/accounts/a/b/leverage"}).has_value());
}

TEST(MarginRoutes, AReadNamesTheInstrumentInTheQuery) {
    Fixture f;
    f.gateway.answer = moved();
    const MarginRoutes routes{f.desk};
    const auto missing = route_now(routes, f.desk, {.method = "GET", .path = "/v1/accounts/binance/leverage"});
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->status, 400);

    const std::string query = "instrument_id=" + std::to_string(k_perp.packed);
    const auto got = route_now(
        routes, f.desk, {.method = "GET", .path = "/v1/accounts/binance/leverage", .query = query});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, 200);
    EXPECT_NE(got->body.find(R"("account":"binance")"), std::string::npos) << got->body;
    EXPECT_NE(got->body.find(R"("leverage":25)"), std::string::npos) << got->body;
    EXPECT_NE(got->body.find(R"("margin_mode":"isolated")"), std::string::npos) << got->body;
    EXPECT_NE(got->body.find(R"("max_leverage":125)"), std::string::npos) << got->body;
    EXPECT_NE(got->body.find(R"("scope":"instrument")"), std::string::npos) << got->body;
    ASSERT_EQ(f.gateway.asked.size(), 1U);
    EXPECT_EQ(f.gateway.asked[0].op, MarginOp::Read);
    EXPECT_TRUE(f.journal.lines.empty());
}

TEST(MarginRoutes, APendingReplyHoldsNoOtherWorkOnTheLoop) {
    Fixture f;
    f.gateway.answer = moved();
    f.gateway.hold = true;
    const MarginRoutes routes{f.desk};
    const std::string id = std::to_string(k_perp.packed);
    Answered first;
    Answered second;
    // both return to the loop at once, unanswered
    ASSERT_TRUE(routes({.method = "PUT",
                        .path = "/v1/accounts/binance/leverage",
                        .body = R"({"instrument_id":")" + id + R"(","leverage":25})"},
                       first.respond()));
    ASSERT_TRUE(routes({.method = "GET", .path = "/v1/accounts/binance/leverage", .query = "instrument_id=" + id},
                       second.respond()));
    EXPECT_EQ(f.gateway.asked.size(), 2U);
    EXPECT_TRUE(f.desk.pending());

    // the loop goes on: a stream tick and further pumps run while the gateway is silent
    int stream_ticks = 0;
    for (int pass = 0; pass < 3; ++pass) {
        ++stream_ticks;
        f.desk.pump();
    }
    EXPECT_EQ(stream_ticks, 3);
    EXPECT_FALSE(first.response.has_value());
    EXPECT_FALSE(second.response.has_value());
    EXPECT_TRUE(f.journal.lines.empty());

    f.gateway.hold = false;
    f.desk.pump();
    EXPECT_FALSE(f.desk.pending());
    ASSERT_TRUE(first.response.has_value());
    ASSERT_TRUE(second.response.has_value());
    EXPECT_EQ(first.response->status, 200);
    EXPECT_EQ(second.response->status, 200);
    ASSERT_EQ(f.journal.lines.size(), 1U);
    EXPECT_NE(f.journal.lines[0].detail.find(R"("outcome":"done")"), std::string::npos);
}

TEST(MarginDesk, AReplyThatNeverLandsIsAnsweredAtItsDeadlineAndJournalled) {
    Fixture f;
    f.gateway.hold = true;
    std::optional<std::expected<MarginView, MarginError>> out;
    f.desk.set("binance", k_perp, {.leverage = 5}, "vlad", [&](auto result) { out = std::move(result); });
    f.desk.pump();
    EXPECT_FALSE(out.has_value());
    f.gateway.fault = MarginFault::NoAnswer;
    f.desk.pump();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->error().status, 504);
    ASSERT_EQ(f.journal.lines.size(), 1U);
    EXPECT_NE(f.journal.lines[0].detail.find(R"("outcome":"no_answer")"), std::string::npos);
}

TEST(MarginRoutes, AnAccountWideSwitchSaysSo) {
    Fixture f;
    f.gateway.answer = moved();
    f.gateway.answer.scope = MarginScope::WholeAccount;
    const MarginRoutes routes{f.desk};
    const std::string body = R"({"instrument_id":")" + std::to_string(k_perp.packed) + R"(","margin_mode":"isolated"})";
    const auto done = route_now(
        routes, f.desk, {.method = "PUT", .path = "/v1/accounts/binance/leverage", .body = body});
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->status, 200);
    EXPECT_NE(done->body.find(R"("scope":"account")"), std::string::npos) << done->body;
}

TEST(MarginRoutes, APutIsValidatedThenAttributedToTheLogin) {
    Fixture f;
    f.gateway.answer = moved();
    const MarginRoutes routes{f.desk};
    const std::string id = std::to_string(k_perp.packed);
    const auto put = [&](const std::string& body, std::string_view login = {}) {
        return route_now(
            routes, f.desk, {.method = "PUT", .path = "/v1/accounts/binance/leverage", .body = body, .login = login});
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

    const auto other = route_now(routes, f.desk, {.method = "DELETE", .path = "/v1/accounts/binance/leverage"});
    EXPECT_EQ(other->status, 405);
}

} // namespace
} // namespace raikiri::admin
