#include <raikiri/reference/engine.hpp>
#include <raikiri/reference/mid.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

namespace raikiri::reference {
namespace {

constexpr std::uint64_t k_now = 1'800'000'000ULL * 1'000'000'000ULL;
constexpr std::uint64_t k_ms = 1'000'000;
constexpr std::uint64_t k_s = 1'000 * k_ms;

// prices in hundredths of a dollar
FixedE9 cents(std::int64_t c) {
    return FixedE9::from_raw(c * 10'000'000);
}

struct Book {
    VenueId venue{VenueId::Binance};
    std::int64_t bid{10'000};
    std::int64_t ask{10'001};
    MarketType market{MarketType::Spot};
    QuoteCcy ccy{QuoteCcy::Usd};
    std::uint64_t age_ns{0};
    std::int64_t volume_usd{1'000'000};
};

Quote quote(const Book& b) {
    return Quote{.venue = b.venue,
                 .market = b.market,
                 .ccy = b.ccy,
                 .bid = cents(b.bid),
                 .ask = cents(b.ask),
                 .bid_qty = FixedE9::from_raw(k_e9),
                 .ask_qty = FixedE9::from_raw(k_e9),
                 .ts_ns = k_now - b.age_ns,
                 .volume_24h = FixedE6::from_raw(b.volume_usd * k_e6)};
}

// stablecoin books in ten-thousandths of a dollar
Quote stable(VenueId venue, std::int64_t bid, std::int64_t ask, QuoteCcy ccy = QuoteCcy::Usd) {
    auto q = quote({.venue = venue, .ccy = ccy});
    q.bid = FixedE9::from_raw(bid * 100'000);
    q.ask = FixedE9::from_raw(ask * 100'000);
    return q;
}

std::vector<Quote> five_at(std::int64_t bid_cents) {
    std::vector<Quote> out;
    for (const auto venue : {VenueId::Binance, VenueId::Okx, VenueId::Bybit, VenueId::Coinbase, VenueId::Kraken}) {
        out.push_back(quote({.venue = venue, .bid = bid_cents, .ask = bid_cents + 2}));
    }
    return out;
}

std::uint16_t cuts(const Reference& r, Cut cut) {
    return r.cuts[static_cast<std::size_t>(cut)];
}

TEST(ReferenceMid, AnOutlierIsExcludedAndReadmittedOnlyBelowHalfTheThreshold) {
    const Params params{};
    const StableRates rates{};
    AssetState state;

    auto books = five_at(10'000);
    books[4] = quote({.venue = VenueId::Kraken, .bid = 11'000, .ask = 11'002});
    auto r = compute(books, rates, params, state, k_now);
    ASSERT_FALSE(r.refused);
    EXPECT_EQ(cuts(r, Cut::Outlier), 1);
    EXPECT_EQ(r.used_count(), 4U);
    EXPECT_EQ(r.mid, cents(10'001));

    // 1.5% off: inside the threshold, but not inside half of it
    books[4] = quote({.venue = VenueId::Kraken, .bid = 10'150, .ask = 10'152});
    r = compute(books, rates, params, state, k_now);
    EXPECT_EQ(cuts(r, Cut::Outlier), 1);

    books[4] = quote({.venue = VenueId::Kraken, .bid = 10'050, .ask = 10'052});
    r = compute(books, rates, params, state, k_now);
    EXPECT_EQ(cuts(r, Cut::Outlier), 0);
    EXPECT_EQ(r.used_count(), 5U);

    // a fresh source 1.5% off was never excluded, so it stays in
    AssetState fresh;
    r = compute(books, rates, params, fresh, k_now);
    books[4] = quote({.venue = VenueId::Kraken, .bid = 10'150, .ask = 10'152});
    r = compute(books, rates, params, fresh, k_now);
    EXPECT_EQ(cuts(r, Cut::Outlier), 0);
}

TEST(ReferenceMid, StaleBooksAreCutWithTheSpotAndPerpLimitsApart) {
    const Params params{};
    AssetState state;
    auto books = five_at(10'000);
    books[0] = quote({.venue = VenueId::Binance, .age_ns = 11 * k_s});
    books[1] = quote({.venue = VenueId::Okx, .age_ns = 9 * k_s});
    books.push_back(quote({.venue = VenueId::Bybit, .market = MarketType::Perp, .age_ns = 3 * k_s}));
    books.push_back(quote({.venue = VenueId::Okx, .market = MarketType::Perp, .age_ns = 1 * k_s}));
    const auto r = compute(books, StableRates{}, params, state, k_now);
    EXPECT_EQ(cuts(r, Cut::Stale), 2);
    EXPECT_EQ(cuts(r, Cut::Warming), 1);
    EXPECT_EQ(r.oldest_age_ns, 9 * k_s);

    Quote unstamped = quote({});
    unstamped.ts_ns = 0;
    const std::vector<Quote> one{unstamped};
    EXPECT_EQ(cuts(compute(one, StableRates{}, params, state, k_now), Cut::Stale), 1);
}

TEST(ReferenceMid, CrossedLockedEmptyAndWideBooksAreCut) {
    const Params params{};
    AssetState state;
    auto books = five_at(10'000);
    books.push_back(quote({.venue = VenueId::Gate, .bid = 10'002, .ask = 10'001}));
    books.push_back(quote({.venue = VenueId::Mexc, .bid = 10'001, .ask = 10'001}));
    books.push_back(quote({.venue = VenueId::Htx, .bid = 9'950, .ask = 10'050}));
    books.push_back(quote({.venue = VenueId::Bitget, .bid = 0, .ask = 10'001}));
    const auto r = compute(books, StableRates{}, params, state, k_now);
    EXPECT_EQ(cuts(r, Cut::Crossed), 2);
    EXPECT_EQ(cuts(r, Cut::Wide), 1);
    EXPECT_EQ(cuts(r, Cut::Empty), 1);
    EXPECT_EQ(r.used_count(), 5U);

    // 50 cents over a 100.25 mid is just inside 50 bp
    std::vector<Quote> edge{quote({.venue = VenueId::Htx, .bid = 10'000, .ask = 10'050})};
    EXPECT_EQ(cuts(compute(edge, StableRates{}, params, state, k_now), Cut::Wide), 0);
}

TEST(ReferenceMid, StablecoinBooksSetTheRateAndADepegConvertsTheTetherBooks) {
    const Params params{};
    const std::vector<Quote> usdt{stable(VenueId::Kraken, 9'699, 9'701),
                                  stable(VenueId::Coinbase, 9'698, 9'702),
                                  stable(VenueId::Bitstamp, 9'699, 9'703)};
    const auto rates = stable_rates(usdt, {}, params, k_now);
    EXPECT_FALSE(rates.usdt_fallback);
    EXPECT_EQ(rates.usdt_sources, 3);
    EXPECT_EQ(rates.usdt_usd, FixedE9::from_raw(970'000'000));
    EXPECT_TRUE(rates.usdc_fallback);

    // usdt books quote 103.10 for what usd books quote 100.00
    std::vector<Quote> books{quote({.venue = VenueId::Coinbase, .bid = 10'000, .ask = 10'002}),
                             quote({.venue = VenueId::Kraken, .bid = 10'000, .ask = 10'002}),
                             quote({.venue = VenueId::Binance, .bid = 10'309, .ask = 10'311, .ccy = QuoteCcy::Usdt}),
                             quote({.venue = VenueId::Okx, .bid = 10'309, .ask = 10'311, .ccy = QuoteCcy::Usdt})};
    AssetState state;
    auto r = compute(books, rates, params, state, k_now);
    ASSERT_FALSE(r.refused);
    EXPECT_EQ(cuts(r, Cut::Outlier), 0);
    EXPECT_EQ(r.rates.usdt_usd, rates.usdt_usd);
    EXPECT_LT(std::abs(r.mid.raw - cents(10'001).raw), cents(1).raw);

    // at the declared fallback of one the same books sit 3% apart and the mid lands between them
    AssetState blind;
    r = compute(books, StableRates{}, params, blind, k_now);
    EXPECT_TRUE(r.rates.usdt_fallback);
    EXPECT_GT(r.mid.raw, cents(10'150).raw);
}

TEST(ReferenceMid, UsdcComesFromUsdBooksAndFromUsdcTetherTimesTheTetherRate) {
    const Params params{};
    const std::vector<Quote> usdt{stable(VenueId::Kraken, 9'999, 10'001)};
    const std::vector<Quote> usdc{stable(VenueId::Binance, 9'949, 9'951, QuoteCcy::Usdt),
                                  stable(VenueId::Bitstamp, 9'949, 9'951)};
    const auto rates = stable_rates(usdt, usdc, params, k_now);
    EXPECT_EQ(rates.usdc_sources, 2);
    EXPECT_FALSE(rates.usdc_fallback);
    EXPECT_EQ(rates.usdc_usd, FixedE9::from_raw(995'000'000));

    // no usd book of tether: the declared fallback, and usdc/usdt does not lean on it
    Params declared{};
    declared.usdt_fallback = FixedE9::from_raw(999'000'000);
    const auto blind = stable_rates({}, usdc, declared, k_now);
    EXPECT_TRUE(blind.usdt_fallback);
    EXPECT_EQ(blind.usdt_usd, FixedE9::from_raw(999'000'000));
    EXPECT_EQ(blind.usdc_sources, 1);

    // a stablecoin printed at 0.50 is a broken book
    const std::vector<Quote> broken{stable(VenueId::Kraken, 4'999, 5'001)};
    EXPECT_TRUE(stable_rates(broken, {}, params, k_now).usdt_fallback);
}

TEST(ReferenceMid, AnUnknownQuoteCurrencyIsCutForWantOfARate) {
    EXPECT_EQ(quote_ccy_of("BTC/USDT"), QuoteCcy::Usdt);
    EXPECT_EQ(quote_ccy_of("xyz:TSLA/USDC"), QuoteCcy::Usdc);
    EXPECT_EQ(quote_ccy_of("BTC/USD-PERP"), QuoteCcy::Usd);
    EXPECT_EQ(quote_ccy_of("BTC/KRW"), QuoteCcy::Other);
    EXPECT_EQ(quote_ccy_of("BTCUSDT"), QuoteCcy::Other);

    auto books = five_at(10'000);
    books.push_back(quote({.venue = VenueId::Upbit, .ccy = QuoteCcy::Other}));
    AssetState state;
    const auto r = compute(books, StableRates{}, Params{}, state, k_now);
    EXPECT_EQ(cuts(r, Cut::NoRate), 1);
}

TEST(ReferenceMid, FewerThanTheMinimumSourcesIsARefusal) {
    const Params params{};
    AssetState state;
    auto books = five_at(10'000);
    books.resize(2);
    auto r = compute(books, StableRates{}, params, state, k_now);
    EXPECT_TRUE(r.refused);
    EXPECT_TRUE(r.mid.is_undef());
    EXPECT_TRUE(r.conf.is_undef());
    EXPECT_FALSE(r.spot_mid.is_undef());

    books = five_at(10'000);
    books.resize(3);
    r = compute(books, StableRates{}, params, state, k_now);
    EXPECT_FALSE(r.refused);
    EXPECT_EQ(r.mid, cents(10'001));

    // a second book of one venue does not count twice
    books[2].venue = VenueId::Binance;
    r = compute(books, StableRates{}, params, state, k_now);
    EXPECT_TRUE(r.refused);
    EXPECT_EQ(cuts(r, Cut::Duplicate), 1);

    EXPECT_TRUE(compute({}, StableRates{}, params, state, k_now).refused);
}

TEST(ReferenceMid, ConfidenceIsTheLargerOfHalfTheQuartileRangeAndTheMedianHalfSpread) {
    const Params params{};
    AssetState state;
    // mids 100.01, 100.03, 100.05, 100.07 at equal weight; half-spreads of a cent
    std::vector<Quote> books{quote({.venue = VenueId::Binance, .bid = 10'000, .ask = 10'002}),
                             quote({.venue = VenueId::Okx, .bid = 10'002, .ask = 10'004}),
                             quote({.venue = VenueId::Bybit, .bid = 10'004, .ask = 10'006}),
                             quote({.venue = VenueId::Coinbase, .bid = 10'006, .ask = 10'008})};
    auto r = compute(books, StableRates{}, params, state, k_now);
    ASSERT_FALSE(r.refused);
    // every quantile here falls on an exact split: q25 100.02, median 100.04, q75 100.06
    EXPECT_EQ(r.mid, cents(10'004));
    EXPECT_EQ(r.conf, cents(2));
    // deviations 3, 1, 1, 3 cents: the median is two cents, about 2 bp of the mid
    EXPECT_EQ(r.dispersion_e9, FixedE9::from_raw(mul_div(cents(2).raw, k_e9, cents(10'004).raw)));

    // identical mids: the spread alone sets the confidence and dispersion is zero
    AssetState tight;
    r = compute(five_at(10'000), StableRates{}, params, tight, k_now);
    EXPECT_EQ(r.conf, cents(1));
    EXPECT_EQ(r.dispersion_e9, FixedE9::from_raw(0));
}

TEST(ReferenceMid, VolumeWeightsAreCappedAtThreeTimesTheMedian) {
    const Params params{};
    AssetState state;
    std::vector<Quote> books{
        quote({.venue = VenueId::Binance, .bid = 10'000, .ask = 10'002, .volume_usd = 1'000'000'000}),
        quote({.venue = VenueId::Okx, .bid = 10'010, .ask = 10'012}),
        quote({.venue = VenueId::Bybit, .bid = 10'010, .ask = 10'012}),
        quote({.venue = VenueId::Coinbase, .bid = 10'020, .ask = 10'022}),
        quote({.venue = VenueId::Kraken, .bid = 10'020, .ask = 10'022, .volume_usd = 0})};
    const auto r = compute(books, StableRates{}, params, state, k_now);
    ASSERT_FALSE(r.refused);
    // capped, binance holds 3 of 7 and the median stays with the okx and bybit cluster
    EXPECT_EQ(r.mid, cents(10'011));
    for (const auto& used : r.used) {
        EXPECT_LE(used.weight, 3'000'000);
        EXPECT_GE(used.weight, 1'000'000);
    }
}

TEST(ReferenceMid, PerpsEnterThroughTheirBasisToSpotOnceItIsWarm) {
    Params params{};
    params.basis_warm_ns = 10 * k_s;
    AssetState state;
    auto books = five_at(10'000);
    books.resize(3);
    // the perp trades 50 bp over spot
    books.push_back(quote({.venue = VenueId::Bybit, .bid = 10'050, .ask = 10'052, .market = MarketType::Perp}));
    // warmth is judged on the previous update, so the eleventh second is the first admitted
    for (std::uint64_t t = 0; t <= 11; ++t) {
        auto at = books;
        for (auto& q : at) {
            q.ts_ns = k_now + t * k_s;
        }
        const auto r = compute(at, StableRates{}, params, state, k_now + t * k_s);
        if (t < 11) {
            EXPECT_EQ(cuts(r, Cut::Warming), 1) << t;
            EXPECT_EQ(r.used_perp, 0);
        } else {
            EXPECT_EQ(cuts(r, Cut::Warming), 0);
            EXPECT_EQ(r.used_perp, 1);
            EXPECT_EQ(r.mid, cents(10'001));
            EXPECT_EQ(r.perp_mid, cents(10'051));
            // (100.51 / 100.01 - 1) is just under 50 bp
            EXPECT_NEAR(static_cast<double>(r.basis_e9.raw), 4'999'500.0, 1'000.0);
            ASSERT_EQ(r.used.size(), 4U);
            EXPECT_LT(std::abs(r.used.back().price.raw - cents(10'001).raw), cents(1).raw / 10);
        }
    }
}

TEST(ReferenceMid, AnExactHalfSplitAveragesItsNeighbours) {
    std::vector<internal::Weighted> points{{.price = 30, .weight = 1}, {.price = 10, .weight = 1}};
    EXPECT_EQ(internal::weighted_quantile(points, 1, 2), 20);
    points = {{.price = 10, .weight = 1}, {.price = 20, .weight = 2}, {.price = 30, .weight = 1}};
    EXPECT_EQ(internal::weighted_quantile(points, 1, 2), 20);
    EXPECT_EQ(internal::plain_median({5, 1, 3, 7}), 4);
}

class FakeSource final : public QuoteSource {
public:
    std::vector<AssetQuotes> book;

    std::vector<AssetQuotes> snapshot() override { return book; }
};

class FakeSink final : public ReferencePublisher {
public:
    std::vector<Row> rows;

    void publish(std::span<const Row> got, const CycleReport&) override { rows.assign(got.begin(), got.end()); }
};

TEST(ReferenceEngine, PinnedAssetsLeadAndTheUniverseOnlyGrows) {
    FakeSource source;
    FakeSink sink;
    auto busy = five_at(10'000);
    for (auto& q : busy) {
        q.volume_24h = FixedE6::from_raw(5'000'000 * k_e6);
    }
    source.book = {{.asset = "crypto:BTC", .quotes = five_at(10'000)},
                   {.asset = "crypto:DOGE", .quotes = busy},
                   {.asset = "crypto:USDT", .quotes = busy},
                   {.asset = "crypto:THIN", .quotes = {quote({})}}};
    std::uint64_t now = k_now;
    ComputeReferences cycle{
        source, sink, Params{}, Universe{.pinned = {"crypto:BTC", "crypto:ETH"}, .top_n = 5}, [&now] { return now; }};
    (void)cycle();
    const std::vector<std::string> first{"crypto:BTC", "crypto:ETH", "crypto:DOGE"};
    EXPECT_EQ(cycle.members(), first);
    ASSERT_EQ(sink.rows.size(), 3U);
    EXPECT_FALSE(sink.rows[0].ref.refused);
    EXPECT_TRUE(sink.rows[1].ref.refused);

    source.book.pop_back();
    source.book.erase(source.book.begin() + 1);
    now += 2 * k_rerank_ns;
    (void)cycle();
    EXPECT_EQ(cycle.members(), first);
}

} // namespace
} // namespace raikiri::reference
