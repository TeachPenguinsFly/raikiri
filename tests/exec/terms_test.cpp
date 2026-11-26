#include <raikiri/exec/terms.hpp>

#include <gtest/gtest.h>

#include <optional>

namespace raikiri::exec {
namespace {

constexpr UnixNanos k_now{1'700'000'000'000'000'000ULL};
const InstrumentId k_btc = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
const InstrumentId k_okx = InstrumentId::make(VenueId::Okx, MarketType::Perp, SymbolId{1});

class FakeVenue final : public TermsSource {
public:
    std::expected<TradingTerms, VenueError> trading_terms(InstrumentId) override {
        ++calls;
        if (error.has_value()) {
            return std::unexpected{*error};
        }
        return terms;
    }

    TradingTerms terms{};
    std::optional<VenueError> error;
    int calls{0};
};

TermsRequest ask(InstrumentId instrument) {
    return {.id = 5, .instrument = instrument, .ts_created = k_now};
}

TEST(ServeTerms, AnAccountAnswerIsPassedOnWithItsTiers) {
    FakeVenue venue;
    venue.terms = {.maker_fee = 0.0001,
                   .taker_fee = 0.0003,
                   .fee_source = FeeSource::Account,
                   .tiers_source = TiersSource::Venue,
                   .basis = TierBasis::Notional,
                   .tiers = {{.floor = 0, .cap = 10'000, .rate = 0.0065, .amount = 0, .max_leverage = 75}}};
    const auto parts = serve_terms(&venue, ask(k_btc), k_now);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0].id, 5U);
    EXPECT_EQ(parts[0].outcome, TermsOutcome::Done);
    EXPECT_EQ(parts[0].fee_source, FeeSource::Account);
    EXPECT_DOUBLE_EQ(parts[0].taker_fee, 0.0003);
    EXPECT_EQ(parts[0].tiers_source, TiersSource::Venue);
    ASSERT_EQ(parts[0].count, 1U);
    EXPECT_EQ(parts[0].tiers[0], venue.terms.tiers[0]);
}

TEST(ServeTerms, WithoutAnAccountAnswerTheFeesAreTheSchedule) {
    FakeVenue venue;
    const auto parts = serve_terms(&venue, ask(k_btc), k_now);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0].outcome, TermsOutcome::Done);
    EXPECT_EQ(parts[0].fee_source, FeeSource::Schedule);
    const auto base = base_fee_schedule(VenueId::Binance, MarketType::Perp);
    ASSERT_TRUE(base.has_value());
    EXPECT_DOUBLE_EQ(parts[0].maker_fee, base->maker);
    EXPECT_DOUBLE_EQ(parts[0].taker_fee, base->taker);
    EXPECT_EQ(parts[0].tiers_source, TiersSource::Default);
    EXPECT_EQ(parts[0].count, 0U);
}

TEST(ServeTerms, AVenueWithoutTheCallsStillAnswersItsSchedule) {
    const auto parts = serve_terms(nullptr, ask(k_btc), k_now);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0].outcome, TermsOutcome::Done);
    EXPECT_EQ(parts[0].fee_source, FeeSource::Schedule);
}

TEST(ServeTerms, NoScheduleRowAndNoAccountAnswerIsUnsupported) {
    ASSERT_FALSE(base_fee_schedule(VenueId::Okx, MarketType::Perp).has_value());
    FakeVenue venue;
    const auto parts = serve_terms(&venue, ask(k_okx), k_now);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0].outcome, TermsOutcome::Unsupported);
}

TEST(ServeTerms, AVenueFailureIsPassedOnWithItsClass) {
    FakeVenue venue;
    venue.error = VenueError::make(ErrorClass::RateLimited, 250, "local rate budget spent");
    const auto parts = serve_terms(&venue, ask(k_btc), k_now);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0].outcome, TermsOutcome::Failed);
    EXPECT_EQ(parts[0].cls, ErrorClass::RateLimited);
    EXPECT_EQ(parts[0].reason(), "local rate budget spent");
}

TEST(ServeTerms, AnExpiredRequestNeverReachesTheVenue) {
    FakeVenue venue;
    const UnixNanos late{k_now.ns + k_terms_request_ttl_ns + 1};
    const auto parts = serve_terms(&venue, ask(k_btc), late);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0].outcome, TermsOutcome::Failed);
    EXPECT_EQ(venue.calls, 0);
}

TEST(ServeTerms, ALongTableIsCutIntoPartsInOrder) {
    FakeVenue venue;
    venue.terms.fee_source = FeeSource::Account;
    venue.terms.tiers_source = TiersSource::Venue;
    for (int i = 0; i < 19; ++i) {
        venue.terms.tiers.push_back(
            {.floor = i * 100.0, .cap = (i + 1) * 100.0, .rate = 0.01, .amount = 0, .max_leverage = 0});
    }
    const auto parts = serve_terms(&venue, ask(k_btc), k_now);
    ASSERT_EQ(parts.size(), 3U);
    std::size_t seen = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        EXPECT_EQ(parts[i].part, i);
        EXPECT_EQ(parts[i].parts, 3U);
        for (std::size_t k = 0; k < parts[i].count; ++k) {
            EXPECT_EQ(parts[i].tiers[k], venue.terms.tiers[seen++]);
        }
    }
    EXPECT_EQ(seen, 19U);
}

TEST(ServeTerms, ATableLongerThanTheRingHoldsKeepsItsLowestTiers) {
    FakeVenue venue;
    venue.terms.fee_source = FeeSource::Account;
    venue.terms.tiers.assign(k_terms_max_tiers + 40, MaintenanceTier{});
    const auto parts = serve_terms(&venue, ask(k_btc), k_now);
    EXPECT_EQ(parts.size(), k_terms_max_tiers / k_terms_tiers_per_part);
}

} // namespace
} // namespace raikiri::exec
