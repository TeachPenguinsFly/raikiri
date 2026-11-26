#include <raikiri/admin/terms.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace raikiri::admin {
namespace {

constexpr std::uint64_t k_start_ns = 1'700'000'000'000'000'000ULL;
const InstrumentId k_perp = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{3});
const InstrumentId k_other = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{4});
const InstrumentId k_unlisted = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{9});

class FakeGateway final : public TermsGateway {
public:
    std::optional<MarginTarget> target(InstrumentId instrument) const override {
        if (instrument == k_perp || instrument == k_other) {
            return MarginTarget{.account = "binance", .symbol = "BTCUSDT"};
        }
        return std::nullopt;
    }

    double price(InstrumentId) const override { return mid; }

    std::expected<void, MarginFault> send(TermsRequest& request) override {
        if (fault.has_value() && *fault != MarginFault::NoAnswer) {
            return std::unexpected{*fault};
        }
        request.id = asked.size() + 1;
        asked.push_back(request);
        return {};
    }

    std::expected<std::optional<TermsAnswer>, MarginFault> collect(std::uint64_t) override {
        if (fault == MarginFault::NoAnswer) {
            return std::unexpected{*fault};
        }
        if (hold) {
            return std::optional<TermsAnswer>{};
        }
        return std::optional<TermsAnswer>{answer};
    }

    TermsAnswer answer{};
    std::optional<MarginFault> fault;
    bool hold{false};
    double mid{0};
    std::vector<TermsRequest> asked;
};

TermsAnswer venue_answer() {
    TermsAnswer answer{};
    answer.terms = {.maker_fee = 0.0002,
                    .taker_fee = 0.0005,
                    .fee_source = FeeSource::Account,
                    .tiers_source = TiersSource::Venue,
                    .basis = TierBasis::Notional,
                    .tiers = {{.floor = 0, .cap = 10'000, .rate = 0.0065, .amount = 0, .max_leverage = 75},
                              {.floor = 10'000, .cap = 0, .rate = 0.01, .amount = 35, .max_leverage = 0}}};
    return answer;
}

struct Fixture {
    FakeGateway gateway;
    std::uint64_t now_ns{k_start_ns};
    TermsDesk desk{gateway, [this] { return UnixNanos{now_ns}; }};
    TermsRoutes routes{desk};

    std::optional<http::Response> get(std::string_view account, InstrumentId instrument) {
        std::optional<http::Response> out;
        const std::string path = "/v1/accounts/" + std::string{account} + "/terms";
        const std::string query = "instrument_id=" + std::to_string(instrument.packed);
        const http::Request req{.method = "GET", .path = path, .query = query};
        if (!routes(req, [&out](http::Response r) { out = std::move(r); })) {
            return std::nullopt;
        }
        desk.pump();
        return out;
    }
};

TEST(TermsRoutes, OtherPathsAreLeftToTheRouter) {
    Fixture f;
    EXPECT_FALSE(f.routes({.method = "GET", .path = "/v1/accounts/binance/leverage"}, [](http::Response) {}));
    EXPECT_FALSE(f.routes({.method = "GET", .path = "/v1/accounts"}, [](http::Response) {}));
}

TEST(TermsRoutes, TheAnswerIsDecimalStringsWithAbsentFieldsLeftOut) {
    Fixture f;
    f.gateway.answer = venue_answer();
    const auto response = f.get("binance", k_perp);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 200);
    const std::string id = std::to_string(k_perp.packed);
    EXPECT_EQ(response->body,
              R"({"account":"binance","instrument_id":")" + id +
                  R"(","maker_fee":"0.0002","taker_fee":"0.0005","fee_source":"account","maintenance_tiers":[)"
                  R"({"notional_floor":"0","notional_cap":"10000","maintenance_margin_rate":"0.0065",)"
                  R"("maintenance_amount":"0","max_leverage":"75"},)"
                  R"({"notional_floor":"10000","maintenance_margin_rate":"0.01","maintenance_amount":"35"}],)"
                  R"("tiers_source":"venue","as_of":1700000000000})");
}

TEST(TermsRoutes, BadInputAndUnknownInstrumentsNeverReachTheGateway) {
    Fixture f;
    std::optional<http::Response> out;
    ASSERT_TRUE(f.routes({.method = "GET", .path = "/v1/accounts/binance/terms", .query = "instrument_id=abc"},
                         [&out](http::Response r) { out = std::move(r); }));
    EXPECT_EQ(out->status, 400);
    EXPECT_EQ(f.get("binance", k_unlisted)->status, 404);
    EXPECT_EQ(f.get("bybit", k_perp)->status, 404);
    ASSERT_TRUE(f.routes({.method = "PUT", .path = "/v1/accounts/binance/terms"},
                         [&out](http::Response r) { out = std::move(r); }));
    EXPECT_EQ(out->status, 405);
    EXPECT_TRUE(f.gateway.asked.empty());
}

TEST(TermsDesk, AnAnswerIsKeptForAnHourThenAskedAgain) {
    Fixture f;
    f.gateway.answer = venue_answer();
    EXPECT_EQ(f.get("binance", k_perp)->status, 200);
    f.now_ns += k_terms_ttl_ns - 1;
    const auto kept = f.get("binance", k_perp);
    EXPECT_EQ(kept->status, 200);
    EXPECT_NE(kept->body.find(R"("as_of":1700000000000)"), std::string::npos);
    EXPECT_EQ(f.gateway.asked.size(), 1U);
    f.now_ns += 1;
    EXPECT_EQ(f.get("binance", k_perp)->status, 200);
    EXPECT_EQ(f.gateway.asked.size(), 2U);
    // another instrument is its own row
    EXPECT_EQ(f.get("binance", k_other)->status, 200);
    EXPECT_EQ(f.gateway.asked.size(), 3U);
}

TEST(TermsDesk, APendingReadHoldsNothingAndServesEveryoneWhoAskedMeanwhile) {
    Fixture f;
    f.gateway.answer = venue_answer();
    f.gateway.hold = true;
    int answered = 0;
    const auto done = [&answered](const std::expected<TermsView, MarginError>& result) {
        EXPECT_TRUE(result.has_value());
        ++answered;
    };
    f.desk.read("binance", k_perp, done);
    f.desk.read("binance", k_perp, done);
    EXPECT_EQ(f.gateway.asked.size(), 1U);
    for (int pass = 0; pass < 3; ++pass) {
        f.desk.pump();
    }
    EXPECT_TRUE(f.desk.pending());
    EXPECT_EQ(answered, 0);
    f.gateway.hold = false;
    f.desk.pump();
    EXPECT_FALSE(f.desk.pending());
    EXPECT_EQ(answered, 2);
}

TEST(TermsDesk, FaultsAndOutcomesMapToTheirStatusesAndAreNotKept) {
    Fixture f;
    f.gateway.fault = MarginFault::GatewayGone;
    EXPECT_EQ(f.get("binance", k_perp)->status, 503);
    f.gateway.fault = MarginFault::NotServed;
    EXPECT_EQ(f.get("binance", k_perp)->status, 501);
    f.gateway.fault = MarginFault::NoAnswer;
    EXPECT_EQ(f.get("binance", k_perp)->status, 504);
    f.gateway.fault.reset();
    f.gateway.answer = {.outcome = TermsOutcome::Unsupported, .cls = {}, .reason = "no fee source", .terms = {}};
    EXPECT_EQ(f.get("binance", k_perp)->status, 501);
    f.gateway.answer = {
        .outcome = TermsOutcome::Failed, .cls = ErrorClass::RateLimited, .reason = "budget spent", .terms = {}};
    EXPECT_EQ(f.get("binance", k_perp)->status, 429);
    f.gateway.answer = {
        .outcome = TermsOutcome::Failed, .cls = ErrorClass::Connectivity, .reason = "no response", .terms = {}};
    EXPECT_EQ(f.get("binance", k_perp)->status, 502);
    // none of those was kept: a good answer is asked for and served
    const auto asked = f.gateway.asked.size();
    f.gateway.answer = venue_answer();
    EXPECT_EQ(f.get("binance", k_perp)->status, 200);
    EXPECT_EQ(f.gateway.asked.size(), asked + 1);
}

TEST(TermsDesk, TiersCountedInBaseUnitsArePricedAtEachAnswer) {
    Fixture f;
    f.gateway.answer = venue_answer();
    f.gateway.answer.terms.basis = TierBasis::BaseQty;
    f.gateway.answer.terms.tiers = {{.floor = 0, .cap = 10, .rate = 0.004, .amount = 0, .max_leverage = 100},
                                    {.floor = 10, .cap = 50, .rate = 0.005, .amount = 0, .max_leverage = 66}};
    f.gateway.mid = 100'000;
    auto response = f.get("binance", k_perp);
    EXPECT_NE(response->body.find(R"("notional_floor":"1000000","notional_cap":"5000000")"), std::string::npos);
    // the kept tiers follow the market without another venue read
    f.gateway.mid = 50'000;
    response = f.get("binance", k_perp);
    EXPECT_NE(response->body.find(R"("notional_floor":"500000","notional_cap":"2500000")"), std::string::npos);
    EXPECT_EQ(f.gateway.asked.size(), 1U);
    // without a price they cannot be stated in notional
    f.gateway.mid = 0;
    response = f.get("binance", k_perp);
    EXPECT_NE(response->body.find(R"("maintenance_tiers":[],"tiers_source":"default")"), std::string::npos);
}

TEST(TermsText, DecimalsAreFixedAndShortest) {
    EXPECT_EQ(decimal_text(0.0002), "0.0002");
    EXPECT_EQ(decimal_text(0.00002), "0.00002");
    EXPECT_EQ(decimal_text(-0.00005), "-0.00005");
    EXPECT_EQ(decimal_text(1'200'000'000.0), "1200000000");
    EXPECT_EQ(decimal_text(1.0 / 6.0), "0.166666666667");
    EXPECT_EQ(decimal_text(1000.01 * 0.01), "10.0001");
    EXPECT_EQ(decimal_text(0.0), "0");
}

} // namespace
} // namespace raikiri::admin
