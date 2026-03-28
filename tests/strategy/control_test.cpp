// The control surface, without a socket: make_control returns a pure function
// of the registry, the factory, the book and the request, which is the
// property that lets a contract test assert the JSON without binding a port.

#include <raikiri/strategy/control.hpp>

#include <gtest/gtest.h>

#include <glaze/glaze.hpp>

#include <memory>
#include <string>

namespace raikiri::strategy {
namespace {

class SilentContext final : public Context {
public:
    std::expected<ClientOrderId, RejectReason> submit(const NewOrder&) noexcept override {
        return std::unexpected{RejectReason::VenueDown};
    }
    bool cancel(ClientOrderId) noexcept override { return false; }
    void cancel_all(InstrumentId) noexcept override {}
    std::optional<Bbo> bbo(InstrumentId) const noexcept override { return std::nullopt; }
    const Instrument* instrument(InstrumentId) const noexcept override { return nullptr; }
    const portfolio::Position* position(InstrumentId) const noexcept override { return nullptr; }
    FixedE9 unrealized(InstrumentId, PriceTicks) const noexcept override {
        return FixedE9::from_raw(0);
    }
    std::optional<ClientOrderId> set_target(const portfolio::PortfolioTarget&) noexcept override {
        return std::nullopt;
    }
    void set_timer(std::uint32_t, std::uint64_t) noexcept override {}
    const Clock& clock() const noexcept override { return clock_; }

private:
    LiveClock clock_;
};

class Quiet final : public Strategy {};

InstrumentBase linear_base(std::uint32_t symbol) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{symbol});
    base.exchange_symbol = *SymbolText::from("BTCUSDT");
    base.local_symbol = base.exchange_symbol;
    base.tick_size = *parse_decimal_e9("0.1");
    base.lot_size = *parse_decimal_e9("0.001");
    base.min_notional = *parse_decimal_e9("1");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return base;
}

struct Harness {
    Harness() {
        (void)register_type<Quiet>(types, "quiet");
        kernel = {.context_for = [this](std::uint16_t) -> Context* { return &context; },
                  .cancel_working = [this](std::uint16_t) { return pulls; },
                  .wall = [] { return UnixNanos{7'000}; }};
    }

    [[nodiscard]] http::Handler router() {
        live = std::make_unique<Registry>(kernel);
        return make_control(*live, types, book, shutdown.get_token());
    }

    // One filled buy, so the book holds something to report.
    void fill(std::uint16_t strategy_id, std::uint32_t symbol) {
        const auto base = linear_base(symbol);
        OrderEvent event{};
        event.type = OrderEventType::Filled;
        event.side = Side::Buy;
        event.instrument = base.id;
        event.client_order_id = ClientOrderId::make(strategy_id, 1);
        event.price = *base.price_to_ticks(*parse_decimal_e9("100"));
        event.qty = *base.qty_to_lots(*parse_decimal_e9("2"));
        event.leaves_qty = QtyLots::from_raw(0);
        event.fee = *parse_decimal_e9("0.25");
        event.ts_event = UnixNanos{5'000};
        ASSERT_TRUE(book.on_fill(base, event, event.qty).has_value());
    }

    Factory types;
    SilentContext context;
    portfolio::PositionBook book;
    Kernel kernel;
    std::stop_source shutdown;
    std::unique_ptr<Registry> live;
    std::size_t pulls{4};
};

http::Request get(std::string_view path) {
    return {.method = "GET", .path = path};
}

http::Request post(std::string_view path, std::string_view body = {}) {
    return {.method = "POST", .path = path, .body = body};
}

TEST(StrategyControl, ItNamesNoStrategyTypeOfItsOwn) {
    // The one rule this surface exists under: a service linking strategies
    // this repository does not contain lists them here without a line
    // changing, so the answer is whatever was registered and nothing else.
    Harness h;
    const auto route = h.router();
    const auto empty = route(get("/v1/strategy-types"));
    EXPECT_EQ(empty.status, 200);
    EXPECT_NE(empty.body.find("quiet"), std::string::npos) << empty.body;

    Factory other;
    ASSERT_TRUE(register_type<Quiet>(other, "something-else-entirely"));
    Registry live{h.kernel};
    const auto second = make_control(live, other, h.book, h.shutdown.get_token());
    const auto listed = second(get("/v1/strategy-types"));
    EXPECT_NE(listed.body.find("something-else-entirely"), std::string::npos) << listed.body;
    EXPECT_EQ(listed.body.find("quiet"), std::string::npos) << listed.body;
}

TEST(StrategyControl, ACreateAnswers201AndDoesNotStartIt) {
    // Two instructions on purpose: creating and starting in one call means a
    // typo in a config is discovered by the market.
    Harness h;
    const auto route = h.router();
    const auto made = route(post("/v1/strategies",
                                 R"({"name":"one","type":"quiet","config":"","strategy_id":9})"));
    ASSERT_EQ(made.status, 201) << made.body;
    EXPECT_NE(made.body.find("\"phase\":\"created\""), std::string::npos) << made.body;
    EXPECT_EQ(h.live->running(), 0U);
}

TEST(StrategyControl, StartAndStopMoveThePhaseAndSayWhatTheStopPulled) {
    Harness h;
    const auto route = h.router();
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"one","type":"quiet","strategy_id":9})"))
                  .status,
              201);

    const auto started = route(post("/v1/strategies/9/start"));
    ASSERT_EQ(started.status, 200) << started.body;
    EXPECT_NE(started.body.find("\"phase\":\"running\""), std::string::npos) << started.body;

    const auto stopped = route(post("/v1/strategies/9/stop"));
    ASSERT_EQ(stopped.status, 200) << stopped.body;
    EXPECT_NE(stopped.body.find("\"phase\":\"stopped\""), std::string::npos) << stopped.body;
    // Orders pulled, never lots sold: a stop does not flatten.
    EXPECT_NE(stopped.body.find("\"orders_cancelled\":4"), std::string::npos) << stopped.body;
}

TEST(StrategyControl, AnIllegalTransitionIsAConflictAndAMissingOneIsNotFound) {
    // 404 against 409 is the distinction the two error families exist to keep:
    // "there is no such strategy" and "there is, and it is already stopped"
    // are answered differently by everything that automates either.
    Harness h;
    const auto route = h.router();
    EXPECT_EQ(route(post("/v1/strategies/9/start")).status, 404);
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"one","type":"quiet","strategy_id":9})"))
                  .status,
              201);
    EXPECT_EQ(route(post("/v1/strategies/9/stop")).status, 409);
    ASSERT_EQ(route(post("/v1/strategies/9/start")).status, 200);
    EXPECT_EQ(route(post("/v1/strategies/9/start")).status, 409);
    ASSERT_EQ(route(post("/v1/strategies/9/stop")).status, 200);
    EXPECT_EQ(route(post("/v1/strategies/9/start")).status, 409);
}

TEST(StrategyControl, EveryRefusalOfACreateCarriesTheStatusItDeserves) {
    Harness h;
    const auto route = h.router();
    // Not JSON at all.
    EXPECT_EQ(route(post("/v1/strategies", "not json")).status, 400);
    // A type nobody registered: the request is wrong and will be wrong again.
    EXPECT_EQ(
        route(post("/v1/strategies", R"({"name":"x","type":"nope","strategy_id":9})")).status,
        422);
    // The reserved ids.
    EXPECT_EQ(route(post("/v1/strategies", R"({"name":"x","type":"quiet","strategy_id":0})"))
                  .status,
              422);
    EXPECT_EQ(route(post("/v1/strategies", R"({"name":"x","type":"quiet","strategy_id":1})"))
                  .status,
              422);
    // Out of range for the 16 bits a ClientOrderId carries.
    EXPECT_EQ(route(post("/v1/strategies", R"({"name":"x","type":"quiet","strategy_id":70000})"))
                  .status,
              422);
    // And an id already held is a conflict with the state, not a bad request.
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"x","type":"quiet","strategy_id":9})"))
                  .status,
              201);
    EXPECT_EQ(route(post("/v1/strategies", R"({"name":"y","type":"quiet","strategy_id":9})"))
                  .status,
              409);
}

TEST(StrategyControl, ACreateNamesTheFieldItIsMissing) {
    // Measured against a live strategyd: a body without strategy_id was told
    // its id was "not a whole number", and so was one whose type was unknown.
    Harness h;
    const auto route = h.router();
    const auto no_id = route(post("/v1/strategies", R"({"type":"quiet"})"));
    EXPECT_EQ(no_id.status, 422);
    EXPECT_NE(no_id.body.find("strategy_id is required"), std::string::npos) << no_id.body;
    const auto no_type = route(post("/v1/strategies", R"({"strategy_id":9})"));
    EXPECT_EQ(no_type.status, 422);
    EXPECT_NE(no_type.body.find("type is required"), std::string::npos) << no_type.body;
}

TEST(StrategyControl, ARefusalIsRfc9457) {
    Harness h;
    const auto route = h.router();
    const auto refused = route(post("/v1/strategies", R"({"type":"nope","strategy_id":9})"));
    EXPECT_EQ(refused.content_type, "application/problem+json");
    EXPECT_NE(refused.body.find("\"status\":422"), std::string::npos) << refused.body;
    // The detail names the type that was asked for, so a caller that mistyped
    // one sees which spelling arrived.
    EXPECT_NE(refused.body.find("nope"), std::string::npos) << refused.body;
}

TEST(StrategyControl, PositionsAndPnlAreServedPerStrategyAndNothingElseLeaksIn) {
    Harness h;
    const auto route = h.router();
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"mine","type":"quiet","strategy_id":9})"))
                  .status,
              201);
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"theirs","type":"quiet","strategy_id":8})"))
                  .status,
              201);
    h.fill(9, 1);
    h.fill(8, 2);

    const auto mine = route(get("/v1/strategies/9/positions"));
    ASSERT_EQ(mine.status, 200) << mine.body;
    const auto ours = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});
    const auto theirs = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{2});
    EXPECT_NE(mine.body.find(std::to_string(ours.packed)), std::string::npos) << mine.body;
    EXPECT_EQ(mine.body.find(std::to_string(theirs.packed)), std::string::npos)
        << "another strategy's position was served: " << mine.body;

    // AND THE ID IS DIGITS IN A STRING. It carries the venue at bit 56 and
    // would reach a browser rounded as a JSON number.
    EXPECT_NE(mine.body.find("\"" + std::to_string(ours.packed) + "\""), std::string::npos)
        << mine.body;

    const auto pnl = route(get("/v1/strategies/9/pnl"));
    ASSERT_EQ(pnl.status, 200) << pnl.body;
    EXPECT_NE(pnl.body.find("realized"), std::string::npos) << pnl.body;
    EXPECT_NE(pnl.body.find("fees"), std::string::npos) << pnl.body;
    // NO TOTAL. Realized PnL is denominated in each instrument's settlement
    // currency, so a sum across them would be adding unlike things.
    EXPECT_EQ(pnl.body.find("total"), std::string::npos) << pnl.body;
}

TEST(StrategyControl, PositionsForAStrategyThisProcessDoesNotRunIsNotAnEmptyPage) {
    // The book may hold a position under an id restored from the journal, and
    // answering with rows would say this process runs a strategy by it.
    Harness h;
    const auto route = h.router();
    h.fill(31, 1);
    EXPECT_EQ(route(get("/v1/strategies/31/positions")).status, 404);
    EXPECT_EQ(route(get("/v1/strategies/31/pnl")).status, 404);
    EXPECT_EQ(route(get("/v1/strategies/31")).status, 404);
}

TEST(StrategyControl, AListingCountsWhatEachStrategyHolds) {
    Harness h;
    const auto route = h.router();
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"mine","type":"quiet","strategy_id":9})"))
                  .status,
              201);
    h.fill(9, 1);
    const auto listed = route(get("/v1/strategies"));
    ASSERT_EQ(listed.status, 200) << listed.body;
    EXPECT_NE(listed.body.find("\"positions\":1"), std::string::npos) << listed.body;
    EXPECT_NE(listed.body.find("\"name\":\"mine\""), std::string::npos) << listed.body;
}

TEST(StrategyControl, AnIdThatIsNotDigitsIsNotAStrategy) {
    // A partial parse is a refusal: "9x" is not the id 9, it is a client that
    // built a URL wrong.
    Harness h;
    const auto route = h.router();
    EXPECT_EQ(route(get("/v1/strategies/9x")).status, 404);
    EXPECT_EQ(route(get("/v1/strategies/70000")).status, 404);
    EXPECT_EQ(route(post("/v1/strategies/9x/start")).status, 404);
}

TEST(StrategyControl, AWriteMethodOnAReadPathIsRefusedAndAnUnknownPathIsNotFound) {
    Harness h;
    const auto route = h.router();
    EXPECT_EQ(route({.method = "DELETE", .path = "/v1/strategies"}).status, 405);
    EXPECT_EQ(route(get("/v1/nothing-here")).status, 404);
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"x","type":"quiet","strategy_id":9})"))
                  .status,
              201);
    EXPECT_EQ(route(post("/v1/strategies/9/flatten")).status, 404)
        << "a stop does not flatten, and neither does anything else here";
}

TEST(StrategyControl, HealthCountsInstancesRunningAndTypes) {
    Harness h;
    const auto route = h.router();
    const auto empty = route(get("/v1/health"));
    ASSERT_EQ(empty.status, 200) << empty.body;
    EXPECT_NE(empty.body.find("\"instances\":0"), std::string::npos) << empty.body;
    EXPECT_NE(empty.body.find("\"types\":1"), std::string::npos) << empty.body;

    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"x","type":"quiet","strategy_id":9})"))
                  .status,
              201);
    ASSERT_EQ(route(post("/v1/strategies/9/start")).status, 200);
    const auto busy = route(get("/v1/health"));
    EXPECT_NE(busy.body.find("\"instances\":1"), std::string::npos) << busy.body;
    EXPECT_NE(busy.body.find("\"running\":1"), std::string::npos) << busy.body;
}

TEST(StrategyControl, AStrategyStartedHereIsStoppedByTheProcessStopSignal) {
    // Without the process token a strategy started over HTTP would be one that
    // shutting the process down never told to stop.
    Harness h;
    const auto route = h.router();
    ASSERT_EQ(route(post("/v1/strategies", R"({"name":"x","type":"quiet","strategy_id":9})"))
                  .status,
              201);
    ASSERT_EQ(route(post("/v1/strategies/9/start")).status, 200);
    h.shutdown.request_stop();
    EXPECT_EQ(h.live->poll(), 1U);
    EXPECT_EQ(h.live->find(9)->phase, Phase::Stopped);
}

} // namespace
} // namespace raikiri::strategy
