#include <raikiri/log/log.hpp>
#include <raikiri/optionrisk/infra.hpp>
#include <raikiri/optionrisk/queries.hpp>
#include <raikiri/optionrisk/routes.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace raikiri;
using namespace raikiri::optionrisk;

namespace {

constexpr UnixNanos k_now{1'793'606'400'000'000'000ULL};
constexpr std::uint64_t k_day_ns = 86'400ULL * 1'000'000'000ULL;
constexpr UnixNanos k_expiry{k_now.ns + 30 * k_day_ns};

FixedE9 e9(double value) {
    return FixedE9::from_raw(std::llround(value * 1e9));
}

options::OptionKey btc_key(PutCall pc = PutCall::Call) {
    return {.underlying = *UnderlyingText::from("BTC"), .expiry = k_expiry, .strike = e9(100'000.0), .put_call = pc};
}

Holding held(std::uint32_t symbol = 1, double contracts = 1.0) {
    return {.account = "deribit",
            .instrument = InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{symbol}),
            .symbol = *SymbolText::from("BTC-OPT"),
            .key = btc_key(),
            .premium = OptionUnit::Quote,
            .contracts = e9(contracts),
            .contract_value = e9(1.0),
            .entry = e9(5000.0),
            .as_of = k_now};
}

struct FakeHoldings final : HoldingSource {
    std::vector<Holding> rows;
    bool up{true};

    void read(std::vector<Holding>& out) const override { out = rows; }

    [[nodiscard]] bool attached(std::string_view) const override { return up; }
};

struct FakeMarket final : optiondata::OptionMarket {
    std::vector<optiondata::OptionQuote> quotes;

    [[nodiscard]] std::vector<optiondata::UnderlyingListing> underlyings() const override { return {}; }

    void read(std::string_view underlying,
              std::optional<UnixNanos>,
              std::vector<optiondata::OptionQuote>& out) const override {
        out.clear();
        for (const auto& q : quotes) {
            if (q.key.underlying.view() == underlying) {
                out.push_back(q);
            }
        }
    }
};

struct FakeSettlements final : SettlementSource {
    std::optional<SettlementPrice> answer;

    [[nodiscard]] std::optional<SettlementPrice> price(VenueId, std::string_view, UnixNanos) const override {
        return answer;
    }
};

struct MemoryLedger final : SettlementLedger {
    std::vector<SettledPosition> rows;
    int appended{0};

    [[nodiscard]] std::vector<SettledPosition> load() override { return rows; }

    void append(const SettledPosition& settled) override {
        rows.push_back(settled);
        ++appended;
    }
};

struct FakeRealized final : RealizedSource {
    std::vector<BookedFill> rows;

    void read(std::vector<BookedFill>& out) const override { out = rows; }
};

optiondata::OptionQuote quoted(UnixNanos ts = k_now) {
    OptionGreeks g{};
    g.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{1});
    g.mark_iv = e9(0.5);
    g.mark_price = e9(6000.0);
    g.underlying_price = e9(100'000.0);
    g.index_price = e9(99'900.0);
    g.ts_event = ts;
    return {.instrument = g.instrument,
            .key = btc_key(),
            .symbol = *SymbolText::from("BTC-OPT"),
            .premium = OptionUnit::Quote,
            .greeks_unit = OptionUnit::Quote,
            .greeks = g,
            .ts = ts};
}

struct Rig {
    FakeHoldings holdings;
    FakeMarket market;
    FakeSettlements settlements;
    MemoryLedger ledger;
    UnixNanos now{k_now};

    [[nodiscard]] OptionRisk risk(const RealizedSource* realized = nullptr) {
        return OptionRisk{{.holdings = &holdings,
                           .market = &market,
                           .settlements = &settlements,
                           .ledger = &ledger,
                           .realized = realized},
                          [this] { return now; }};
    }
};

} // namespace

TEST(OptionRisk, AssessesAnOpenPositionFromTheMarket) {
    Rig rig;
    rig.holdings.rows = {held()};
    rig.market.quotes = {quoted()};
    auto risk = rig.risk();
    const auto view = risk.view();
    ASSERT_EQ(view.positions.size(), 1U);
    EXPECT_EQ(view.positions[0].market.mark, e9(6000.0));
    EXPECT_EQ(view.positions[0].value_usd, e9(6000.0));
    EXPECT_EQ(view.aggregates.total.positions, 1U);
    EXPECT_TRUE(view.settled.empty());
    EXPECT_FALSE(view.realized_known);
}

TEST(OptionRisk, APositionPastExpirySettlesOnceAndLeavesTheOpenSet) {
    Rig rig;
    rig.holdings.rows = {held()};
    rig.market.quotes = {quoted()};
    auto risk = rig.risk();
    EXPECT_EQ(risk.view().positions.size(), 1U);

    rig.now = UnixNanos{k_expiry.ns + 1};
    rig.settlements.answer = SettlementPrice{.price = e9(110'000.0), .by = SettledBy::Venue, .ts = k_expiry};
    const auto first = risk.view();
    EXPECT_TRUE(first.positions.empty());
    ASSERT_EQ(first.settled.size(), 1U);
    EXPECT_EQ(first.settled[0].realized, e9(5000.0));
    EXPECT_FALSE(first.settled[0].from_last_seen);
    EXPECT_EQ(first.aggregates.total.realized_usd, e9(5000.0));
    EXPECT_EQ(first.aggregates.total.positions, 0U);

    // the venue goes on reporting it for a while
    const auto second = risk.view();
    EXPECT_EQ(second.settled.size(), 1U);
    EXPECT_EQ(rig.ledger.appended, 1);

    // and a restart reads the ledger rather than booking it again
    auto restarted = rig.risk();
    const auto third = restarted.view();
    EXPECT_EQ(third.settled.size(), 1U);
    EXPECT_TRUE(third.positions.empty());
    EXPECT_EQ(rig.ledger.appended, 1);
}

TEST(OptionRisk, AVenueThatClearedThePositionFirstStillSettlesFromTheLastReport) {
    Rig rig;
    rig.holdings.rows = {held()};
    auto risk = rig.risk();
    risk.sweep();
    rig.holdings.rows.clear();
    rig.now = UnixNanos{k_expiry.ns + 1};
    rig.settlements.answer = SettlementPrice{.price = e9(110'000.0), .by = SettledBy::Index, .ts = k_expiry};
    const auto view = risk.view();
    ASSERT_EQ(view.settled.size(), 1U);
    EXPECT_TRUE(view.settled[0].from_last_seen);
    EXPECT_EQ(view.settled[0].realized, e9(5000.0));
}

TEST(OptionRisk, WithNoPriceAnExpiredPositionWaitsOutsideTheBookAndNeverKeepsItsMark) {
    Rig rig;
    rig.holdings.rows = {held()};
    rig.market.quotes = {quoted()};
    auto risk = rig.risk();
    risk.sweep();
    rig.now = UnixNanos{k_expiry.ns + 1};
    const auto waiting = risk.view();
    EXPECT_TRUE(waiting.positions.empty());
    EXPECT_TRUE(waiting.settled.empty());
    ASSERT_EQ(waiting.awaiting_settlement.size(), 1U);
    EXPECT_EQ(waiting.aggregates.total.value_usd.raw, 0);
    EXPECT_EQ(rig.ledger.appended, 0);

    rig.settlements.answer = SettlementPrice{.price = e9(90'000.0), .by = SettledBy::Index, .ts = rig.now};
    const auto done = risk.view();
    EXPECT_TRUE(done.awaiting_settlement.empty());
    ASSERT_EQ(done.settled.size(), 1U);
    EXPECT_EQ(done.settled[0].realized, e9(-5000.0));
}

TEST(OptionRisk, ClosedBeforeExpiryIsForgottenButADetachedGatewayIsNot) {
    Rig rig;
    rig.holdings.rows = {held()};
    rig.market.quotes = {quoted()};
    auto risk = rig.risk();
    risk.sweep();
    rig.holdings.rows.clear();
    rig.holdings.up = false;
    EXPECT_EQ(risk.view().positions.size(), 1U);

    rig.holdings.up = true;
    EXPECT_TRUE(risk.view().positions.empty());
    rig.now = UnixNanos{k_expiry.ns + 1};
    rig.settlements.answer = SettlementPrice{.price = e9(90'000.0), .by = SettledBy::Index, .ts = rig.now};
    EXPECT_TRUE(risk.view().settled.empty());
}

TEST(OptionRisk, RealizedComesFromTheFoldAndCountsFlatContractsOnce) {
    Rig rig;
    FakeRealized realized;
    realized.rows = {
        {.instrument = held().instrument, .key = btc_key(), .premium = OptionUnit::Quote, .realized = e9(100.0)},
        {.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{9}),
         .key = btc_key(PutCall::Put),
         .premium = OptionUnit::Base,
         .realized = e9(0.5)}};
    rig.holdings.rows = {held()};
    rig.market.quotes = {quoted()};
    auto risk = rig.risk(&realized);
    const auto view = risk.view();
    ASSERT_EQ(view.positions.size(), 1U);
    EXPECT_EQ(view.positions[0].realized, e9(100.0));
    EXPECT_TRUE(view.realized_known);
    // 100 usd and 0.5 coin at the 99900 index
    EXPECT_EQ(view.aggregates.total.realized_usd, e9(100.0 + 49'950.0));
}

TEST(OptionRisk, ScenariosTakeOneUnderlying) {
    Rig rig;
    auto eth = held(2);
    eth.key.underlying = *UnderlyingText::from("ETH");
    rig.holdings.rows = {held(), eth};
    rig.market.quotes = {quoted()};
    auto risk = rig.risk();
    const std::vector spot{e9(0.0)};
    const std::vector vol{e9(0.0)};
    const auto m = risk.scenarios("BTC", spot, vol);
    EXPECT_EQ(m.positions, 1U);
    EXPECT_EQ(m.cells[0].value_usd, e9(6000.0));
    EXPECT_EQ(m.cells[0].pnl_usd.raw, 0);
    EXPECT_EQ(risk.scenarios("ETH", spot, vol).positions, 0U);
}

TEST(IndexSettlements, TakesTheFirstIndexAtOrAfterExpiryOnTheVenue) {
    FakeMarket market;
    const IndexSettlements settlements{market};
    market.quotes = {quoted()};
    EXPECT_FALSE(settlements.price(VenueId::Deribit, "BTC", k_expiry).has_value());

    auto after = quoted(UnixNanos{k_expiry.ns + 10});
    after.greeks->index_price = e9(101'000.0);
    auto later = quoted(UnixNanos{k_expiry.ns + 20});
    later.greeks->index_price = e9(102'000.0);
    market.quotes = {later, after, quoted()};
    const auto price = settlements.price(VenueId::Deribit, "BTC", k_expiry);
    ASSERT_TRUE(price.has_value());
    EXPECT_EQ(price->price, e9(101'000.0));
    EXPECT_EQ(price->by, SettledBy::Index);
    EXPECT_FALSE(settlements.price(VenueId::Okx, "BTC", k_expiry).has_value());
}

TEST(FileSettlementLedger, RoundTripsAndSkipsATornLine) {
    const log::Scope logging{"optionrisk_test"};
    const auto path = "/tmp/raikiri-optionrisk-" + std::to_string(::getpid()) + "/settlements.jsonl";
    std::filesystem::remove_all(std::filesystem::path{path}.parent_path());
    FileSettlementLedger ledger{path};
    EXPECT_TRUE(ledger.load().empty());
    auto coin = held();
    coin.premium = OptionUnit::Base;
    coin.entry = e9(0.05);
    const auto booked = settle(coin, {.price = e9(125'000.0), .by = SettledBy::Venue, .ts = k_expiry}, k_expiry, true);
    ledger.append(booked);
    std::ofstream{path, std::ios::app} << "{\"account\":\"torn";
    const auto loaded = ledger.load();
    ASSERT_EQ(loaded.size(), 1U);
    EXPECT_EQ(loaded[0].holding.account, "deribit");
    EXPECT_EQ(loaded[0].holding.instrument, coin.instrument);
    EXPECT_EQ(loaded[0].at.by, SettledBy::Venue);
    EXPECT_TRUE(loaded[0].from_last_seen);
    EXPECT_EQ(loaded[0].realized, booked.realized);
    EXPECT_EQ(loaded[0].realized_usd, booked.realized_usd);
    std::filesystem::remove_all(std::filesystem::path{path}.parent_path());
}

TEST(ShmHoldings, ReadsNonZeroOptionCellsWithTheVenuesSizes) {
    const auto dir = "/tmp/raikiri-optionrisk-shm-" + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        auto owner = shm::SegmentOwner::create(dir + "/gw.okx", shm::k_gateway_segment_bytes, 1, 1);
        ASSERT_TRUE(owner.has_value());
        const auto channels = shm::create_gateway_channels(*owner);
        ASSERT_TRUE(channels.has_value());
        const auto add = [&](std::uint32_t slot, double strike) {
            InstrumentBase base{};
            base.id = InstrumentId::make(VenueId::Okx, MarketType::Option, SymbolId{slot});
            base.exchange_symbol = *SymbolText::from("BTC-USD-OPT");
            base.underlying = *UnderlyingText::from("BTC");
            base.tick_size = e9(0.0001);
            base.lot_size = e9(1.0);
            base.contract_value = e9(0.01);
            base.kind = ContractKind::Inverse;
            EXPECT_TRUE(channels->catalogue
                            ->push(Instrument{OptionInstrument{.base = base,
                                                               .strike = *base.price_to_ticks(e9(strike)),
                                                               .expiry = k_expiry,
                                                               .put_call = PutCall::Call,
                                                               .premium = OptionUnit::Base,
                                                               .greeks = OptionUnit::Quote}})
                            .has_value());
            return base;
        };
        const auto a = add(0, 100'000.0);
        const auto b = add(1, 110'000.0);
        channels->positions->write(0,
                                   shm::PositionCell{.report = {.instrument = a.id,
                                                                .net_qty = *a.qty_to_lots(e9(-3.0)),
                                                                .avg_entry = *a.price_to_ticks(e9(0.05))},
                                                     .ts_pulled = k_now});
        channels->positions->write(
            1, shm::PositionCell{.report = {.instrument = b.id, .net_qty = QtyLots::from_raw(0)}, .ts_pulled = k_now});
        owner->publish();

        admin::Registry registry;
        ASSERT_EQ(registry.discover(dir, 1), 1U);
        const ShmHoldings holdings{registry};
        std::vector<Holding> rows;
        holdings.read(rows);
        ASSERT_EQ(rows.size(), 1U);
        EXPECT_EQ(rows[0].account, "okx");
        EXPECT_EQ(rows[0].contracts, e9(-3.0));
        // three contracts of 0.01 coin, short, in a coin premium; the kind must not make this usd per contract
        EXPECT_EQ(units_of(rows[0]), e9(-0.03));
        EXPECT_TRUE(coin_settled(rows[0]));
        EXPECT_EQ(rows[0].entry, e9(0.05));
        EXPECT_TRUE(holdings.attached("okx"));
        EXPECT_FALSE(holdings.attached("binance"));
    }
    std::filesystem::remove_all(dir);
}

TEST(RiskRoutes, ServesTheRiskAndTheMatrixWithTheWorstCellMarked) {
    Rig rig;
    auto short_call = held(1, -1.0);
    rig.holdings.rows = {short_call};
    rig.market.quotes = {quoted()};
    auto risk = rig.risk();
    const RiskRoutes routes{risk};
    http::Request req{};
    req.method = "GET";
    req.path = "/v1/options/chain";
    EXPECT_FALSE(routes(req).has_value());

    req.path = "/v1/options/risk";
    auto answer = routes(req);
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 200);
    EXPECT_NE(answer->body.find("\"delta_coin\":\"-"), std::string::npos);
    EXPECT_NE(answer->body.find("\"stale\":false"), std::string::npos);

    req.path = "/v1/options/risk/scenarios";
    req.query = "spot=0,10";
    answer = routes(req);
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 400);

    req.query = "underlying=BTC&spot=-10,0,10&vol=0,5";
    answer = routes(req);
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 200);
    EXPECT_NE(answer->body.find("\"spot\":[\"-10\",\"0\",\"10\"]"), std::string::npos);
    EXPECT_NE(answer->body.find("\"worst\":{\"spot\":\"10\",\"vol\":\"5\""), std::string::npos);

    // a browser's URLSearchParams encodes the commas
    req.query = "underlying=BTC&spot=-10%2C0&vol=0%2C5";
    answer = routes(req);
    ASSERT_TRUE(answer.has_value());
    EXPECT_NE(answer->body.find("\"vol\":[\"0\",\"5\"]"), std::string::npos);

    req.query = "underlying=BTC&spot=-100";
    EXPECT_EQ(routes(req)->status, 400);
    req.query = "underlying=BTC&vol=abc";
    EXPECT_EQ(routes(req)->status, 400);
    req.method = "POST";
    EXPECT_EQ(routes(req)->status, 405);
}
