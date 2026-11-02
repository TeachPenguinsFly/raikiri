#include <raikiri/bookrisk/routes.hpp>

#include <gtest/gtest.h>

#include "fakes.hpp"

namespace raikiri::bookrisk {
namespace {

using namespace raikiri::bookrisk::test;

class Routes : public ::testing::Test {
protected:
    FakeKernel kernel;
    MemoryStore store;
    MemoryAudit audit;
    MemoryNotifier notices;
    TestClock clock{UnixNanos{1'800'000'000'000'000'000}};
    RiskDesk desk{kernel, store, audit, notices, clock};
    RiskRoutes routes{desk};

    http::Response get(std::string_view path) { return *routes({.method = "GET", .path = path}); }

    http::Response post(std::string_view path, std::string_view body) {
        return *routes({.method = "POST", .path = path, .body = body});
    }
};

TEST_F(Routes, OtherPathsFallThrough) {
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/strategies"}).has_value());
    EXPECT_FALSE(routes({.method = "GET", .path = "/riskier"}).has_value());
}

TEST_F(Routes, TheOverviewSaysThereIsNoCapitalYet) {
    const auto answer = get("/risk");
    EXPECT_EQ(answer.status, 200);
    EXPECT_EQ(answer.body.find("\"capital\""), std::string::npos) << answer.body;
    EXPECT_NE(answer.body.find(R"("desk_state":"active")"), std::string::npos);
    EXPECT_NE(answer.body.find(R"("id":7,"name":"book-7","state":"active")"), std::string::npos) << answer.body;
}

TEST_F(Routes, ARaiseWithoutConfirmIs409AndNamesWhatItRaises) {
    const auto first = post("/risk/capital", R"({"capital":"100000","by":"vlad"})");
    EXPECT_EQ(first.status, 409);
    EXPECT_EQ(first.content_type, "application/problem+json");
    EXPECT_NE(first.body.find(R"("raises":["capital"])"), std::string::npos) << first.body;

    const auto second = post("/risk/capital", R"({"capital":"100000","by":"vlad","confirm":true})");
    EXPECT_EQ(second.status, 200);
    EXPECT_NE(second.body.find(R"("capital":"100000")"), std::string::npos) << second.body;
    EXPECT_NE(second.body.find(R"("capital_by":"vlad")"), std::string::npos);
}

TEST_F(Routes, LimitsAreShownInDollarsAndPercents) {
    (void)post("/risk/capital", R"({"capital":"100000","by":"vlad","confirm":true})");
    const auto done = post(
        "/risk/books/7/limits",
        R"({"limits":{"order_notional":{"mode":"percent","value":"2"}},"by":"vlad","confirm":true})");
    ASSERT_EQ(done.status, 200) << done.body;
    const auto book = get("/risk/books/7");
    EXPECT_NE(
        book.body.find(
            R"("order_notional":{"set":{"mode":"percent","value":"2"},"source":"book","mode":"percent","effective":"2000","percent":"2"})"),
        std::string::npos)
        << book.body;
    EXPECT_EQ(get("/risk/books/book-7").status, 200);
    EXPECT_EQ(get("/risk/books/nobody").status, 404);
    EXPECT_EQ(get("/risk/books/999").status, 404) << "asking about a book does not make one";
    EXPECT_EQ(get("/risk").body.find("book-999"), std::string::npos);
}

TEST_F(Routes, BadInputIsSaidPlainly) {
    EXPECT_EQ(post("/risk/capital", "{").status, 400);
    EXPECT_EQ(post("/risk/capital", R"({"capital":"lots","by":"vlad"})").status, 422);
    EXPECT_EQ(post("/risk/capital", R"({"capital":"100","confirm":true})").status, 422);
    EXPECT_EQ(post("/risk/books/7/limits", R"({"limits":{"nope":{"mode":"off"}},"by":"vlad"})").status, 422);
    EXPECT_EQ(post("/risk/books/7/limits", R"({"limits":{"order_notional":{"mode":"some"}},"by":"vlad"})").status, 422);
    EXPECT_EQ(post("/risk/books/7/state", R"({"state":"maybe","by":"vlad"})").status, 422);
    EXPECT_EQ(post("/risk/books/seven/state", R"({"state":"active","by":"vlad"})").status, 404);
    EXPECT_EQ(post("/risk/nope", "{}").status, 404);
    EXPECT_EQ(routes({.method = "DELETE", .path = "/risk"})->status, 405);
}

TEST_F(Routes, KillAndResumeRoundTrip) {
    kernel.open[7] = 3;
    const auto killed = post("/risk/books/7/kill", R"({"by":"vlad","reason":"fat finger"})");
    ASSERT_EQ(killed.status, 200) << killed.body;
    EXPECT_EQ(killed.body, R"({"cancelled":3})");
    const auto book = get("/risk/books/7");
    EXPECT_NE(book.body.find(R"("state":"halted")"), std::string::npos);
    EXPECT_NE(book.body.find(R"("reason":"fat finger")"), std::string::npos);

    EXPECT_EQ(post("/risk/books/7/state", R"({"state":"active","by":"vlad"})").status, 409);
    EXPECT_EQ(post("/risk/books/7/state", R"({"state":"active","by":"vlad","confirm":true})").status, 200);

    EXPECT_EQ(post("/risk/kill", R"({"by":"vlad"})").status, 200);
    EXPECT_NE(get("/risk").body.find(R"("desk_state":"halted")"), std::string::npos);
    EXPECT_EQ(post("/risk/resume", R"({"by":"vlad"})").status, 409);
    EXPECT_EQ(post("/risk/resume", R"({"by":"vlad","confirm":true})").status, 200);
}

} // namespace
} // namespace raikiri::bookrisk
