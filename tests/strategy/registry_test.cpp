// The factory, the registry and the order a start and a stop happen in.
//
// No kernel, no sockets: the Context below records what a strategy asked for
// and the Kernel hooks record what the registry asked the kernel for. Every
// property under test is about ORDER and REFUSAL, and both are visible from
// here.

#include <raikiri/strategies/register.hpp>
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/registry.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace raikiri::strategy {
namespace {

// A Context that answers nothing and remembers nothing, because none of these
// tests are about what a strategy can see. It exists so an instance has an
// address to hold.
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

// Everything that happened, in the order it happened, across every instance.
// One log rather than a flag per event: the properties being tested are
// orderings, and a set of booleans cannot express one.
std::vector<std::string>& journal() {
    static std::vector<std::string> entries;
    return entries;
}

class Recorder final : public Strategy {
public:
    explicit Recorder(std::string tag) : tag_{std::move(tag)} {}

    void on_start(Context&) override { journal().push_back(tag_ + ":start"); }
    void on_stop(Context&) override { journal().push_back(tag_ + ":stop"); }
    void on_bbo(Context&, const Bbo&) override { journal().push_back(tag_ + ":bbo"); }
    void on_trade(Context&, const TradeTick&) override { journal().push_back(tag_ + ":trade"); }
    void on_funding(Context&, const FundingUpdate&) override {
        journal().push_back(tag_ + ":funding");
    }
    void on_order_event(Context&, const OrderEvent&) override {
        journal().push_back(tag_ + ":order");
    }
    void on_timer(Context&, std::uint32_t) override { journal().push_back(tag_ + ":timer"); }

private:
    std::string tag_;
};

// A strategy that reads its config and refuses one it cannot use. The only
// form of registration that can say no, and the reason MakeResult carries a
// sentence rather than a code.
class Picky final : public Strategy {
public:
    static MakeResult make(const Params& params) {
        if (params.config != "good") {
            return std::unexpected{"config must be exactly \"good\", got: " +
                                   std::string{params.config}};
        }
        return std::make_unique<Picky>();
    }
};

// A strategy that places nothing and simply exists, for the id rules.
class Quiet final : public Strategy {};

struct Harness {
    Harness() {
        journal().clear();
        (void)types.add("recorder", [](const Params& params) -> MakeResult {
            return std::make_unique<Recorder>("s" + std::to_string(params.strategy_id));
        });
        (void)register_type<Picky>(types, "picky");
        (void)register_type<Quiet>(types, "quiet");
    }

    [[nodiscard]] Kernel kernel() {
        return {.context_for = [this](std::uint16_t) -> Context* { return &context; },
                .cancel_working =
                    [this](std::uint16_t id) {
                        journal().push_back("cancel:" + std::to_string(id));
                        return pulls;
                    },
                .wall = [this] { return UnixNanos{now_ns}; }};
    }

    Factory types;
    SilentContext context;
    std::size_t pulls{3};
    std::uint64_t now_ns{1'000};
};

// ------------------------------------------------------------ factory ----

TEST(StrategyFactory, ANameIsTakenOnceAndTheSecondRegistrationLoses) {
    // Never replaces: two registrations under one name is a link-order
    // accident, and letting the second win makes which strategy runs depend on
    // the order the objects were linked in.
    Factory types;
    EXPECT_TRUE(register_type<Quiet>(types, "carry"));
    EXPECT_FALSE(register_type<Picky>(types, "carry"));
    EXPECT_EQ(types.size(), 1U);
    // And the FIRST one is what still answers.
    EXPECT_TRUE(types.make("carry", {.config = "anything", .strategy_id = 7}).has_value());
}

TEST(StrategyFactory, ANamelessTypeAndANullConstructorAreBothRefused) {
    Factory types;
    EXPECT_FALSE(types.add("", [](const Params&) -> MakeResult { return nullptr; }));
    EXPECT_FALSE(types.add("named", Constructor{}));
    EXPECT_EQ(types.size(), 0U);
}

TEST(StrategyFactory, AnUnknownTypeSaysTheNameItWasAskedFor) {
    // A caller that mistyped a name needs to see which spelling arrived, not
    // that "the type" is unknown.
    Factory types;
    const auto built = types.make("carrry", {});
    ASSERT_FALSE(built.has_value());
    EXPECT_NE(built.error().find("carrry"), std::string::npos) << built.error();
}

TEST(StrategyFactory, AConstructorMayRefuseItsConfigAndSayWhy) {
    Factory types;
    ASSERT_TRUE(register_type<Picky>(types, "picky"));
    const auto refused = types.make("picky", {.config = "nonsense", .strategy_id = 4});
    ASSERT_FALSE(refused.has_value());
    // The words are the strategy's own — this library cannot know which key
    // was wrong, which is the whole reason the error is text.
    EXPECT_NE(refused.error().find("nonsense"), std::string::npos) << refused.error();
    EXPECT_TRUE(types.make("picky", {.config = "good"}).has_value());
}

TEST(StrategyFactory, AConstructorReturningNothingBecomesAReasonRatherThanACrash) {
    Factory types;
    ASSERT_TRUE(types.add("hollow", [](const Params&) -> MakeResult { return nullptr; }));
    const auto built = types.make("hollow", {});
    EXPECT_FALSE(built.has_value());
}

TEST(StrategyFactory, TheStrategiesLibraryRegistersByACallAndNotByAStaticInitializer) {
    // The hazard register.hpp names: a static object in a static library whose
    // translation unit nothing else references is dropped by the linker, and
    // the failure looks exactly like a type that silently does not exist. A
    // call cannot be dropped, and this is what proves the call is the one that
    // does the work.
    Factory types;
    EXPECT_EQ(raikiri::strategies::register_all(types), 1U);
    EXPECT_TRUE(types.contains("demo-maker"));
    EXPECT_TRUE(types.make("demo-maker", {.config = "", .strategy_id = 3}).has_value());
    // And calling it twice adds nothing rather than replacing anything.
    EXPECT_EQ(raikiri::strategies::register_all(types), 0U);
    EXPECT_EQ(types.size(), 1U);
}

TEST(StrategyFactory, TypesComeBackSortedSoAListingCanBeDiffed) {
    Factory types;
    ASSERT_TRUE(register_type<Quiet>(types, "zeta"));
    ASSERT_TRUE(register_type<Quiet>(types, "alpha"));
    ASSERT_TRUE(register_type<Quiet>(types, "mu"));
    EXPECT_EQ(types.types(), (std::vector<std::string>{"alpha", "mu", "zeta"}));
}

// ------------------------------------------------------------ create ----

TEST(StrategyRegistry, TheManualDeskIdIsNeverHandedToAStrategy) {
    // A strategy sharing k_manual_strategy_id would net its position into the
    // desk's book, and the two would then be one position nobody could take
    // apart again.
    Harness h;
    Registry live{h.kernel()};
    const auto made =
        live.create({.name = "impostor", .type = "quiet", .strategy_id = k_manual_strategy_id},
                    h.types);
    ASSERT_FALSE(made.has_value());
    EXPECT_EQ(made.error().error, CreateError::IdReserved);
}

TEST(StrategyRegistry, ZeroIsTheNoOrderSentinelAndNotAStrategy) {
    // ClientOrderId{0} is the universal "no order" and its strategy_id() is 0,
    // so an event carrying no client order id would be delivered to whoever
    // held this id as if it were theirs.
    Harness h;
    Registry live{h.kernel()};
    const auto made = live.create({.name = "zero", .type = "quiet", .strategy_id = 0}, h.types);
    ASSERT_FALSE(made.has_value());
    EXPECT_EQ(made.error().error, CreateError::IdReserved);
}

TEST(StrategyRegistry, TwoInstancesMayNotShareAnId) {
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "a", .type = "quiet", .strategy_id = 9}, h.types));
    const auto twice = live.create({.name = "b", .type = "quiet", .strategy_id = 9}, h.types);
    ASSERT_FALSE(twice.has_value());
    EXPECT_EQ(twice.error().error, CreateError::IdTaken);
    EXPECT_EQ(live.size(), 1U);
}

TEST(StrategyRegistry, AnUnknownTypeIsRefusedBeforeAnyWindowIsOpened) {
    Harness h;
    bool asked = false;
    auto kernel = h.kernel();
    kernel.context_for = [&](std::uint16_t) -> Context* {
        asked = true;
        return &h.context;
    };
    Registry live{std::move(kernel)};
    const auto made = live.create({.name = "x", .type = "no-such-type", .strategy_id = 5}, h.types);
    ASSERT_FALSE(made.has_value());
    EXPECT_EQ(made.error().error, CreateError::UnknownType);
    EXPECT_FALSE(asked) << "a window was opened for a strategy that could not be built";
}

TEST(StrategyRegistry, AKernelWithNoWindowRefusesRatherThanRunningBlind) {
    Harness h;
    auto kernel = h.kernel();
    kernel.context_for = [](std::uint16_t) -> Context* { return nullptr; };
    Registry live{std::move(kernel)};
    const auto made = live.create({.name = "x", .type = "quiet", .strategy_id = 5}, h.types);
    ASSERT_FALSE(made.has_value());
    EXPECT_EQ(made.error().error, CreateError::NoContext);
}

TEST(StrategyRegistry, AConfigTheStrategyRefusesComesBackInItsOwnWords) {
    Harness h;
    Registry live{h.kernel()};
    const auto made = live.create(
        {.name = "p", .type = "picky", .config = "rubbish", .strategy_id = 6}, h.types);
    ASSERT_FALSE(made.has_value());
    EXPECT_EQ(made.error().error, CreateError::Refused);
    EXPECT_NE(made.error().detail.find("rubbish"), std::string::npos) << made.error().detail;
    EXPECT_EQ(live.size(), 0U) << "a refused strategy was still registered";
}

TEST(StrategyRegistry, ItRefusesToGrowPastItsCapacity) {
    // Capacity is fixed because dispatch holds a pointer into the instance
    // table while a hook runs, and a hook that created a strategy over the
    // control surface would reallocate it underneath.
    Harness h;
    Registry live{h.kernel()};
    for (std::uint16_t id = 2; id < 2 + Registry::k_max_instances; ++id) {
        ASSERT_TRUE(live.create({.name = "q", .type = "quiet", .strategy_id = id}, h.types))
            << "id " << id;
    }
    const auto over = live.create({.name = "one-too-many", .type = "quiet", .strategy_id = 900},
                                  h.types);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().error, CreateError::Full);
    EXPECT_EQ(live.size(), Registry::k_max_instances);
}

// ------------------------------------------------------- start and stop ----

TEST(StrategyRegistry, ACreatedStrategyIsNotYetRunningAndSeesNothing) {
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "r", .type = "recorder", .strategy_id = 3}, h.types));
    EXPECT_EQ(live.find(3)->phase, Phase::Created);
    EXPECT_EQ(live.running(), 0U);

    live.on_bbo({});
    live.on_trade({});
    live.on_funding({});
    EXPECT_TRUE(journal().empty()) << "a created strategy was delivered an event";
}

TEST(StrategyRegistry, StopReversesStartExactly) {
    // Start:  phase, then on_start, then the subscription.
    // Stop:   unsubscribe, then on_stop, then the orders.
    //
    // The order is the whole point: unsubscribing first is what keeps a tick
    // polled mid-teardown out of a strategy that is being taken apart, and
    // cancelling after on_stop is what pulls whatever that hook placed.
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "r", .type = "recorder", .strategy_id = 3}, h.types));
    ASSERT_TRUE(live.start(3, {}));
    live.on_bbo({});
    ASSERT_TRUE(live.stop(3));
    live.on_bbo({});

    EXPECT_EQ(journal(),
              (std::vector<std::string>{"s3:start", "s3:bbo", "s3:stop", "cancel:3"}));
}

TEST(StrategyRegistry, AStopPullsWorkingOrdersAndReportsHowMany) {
    Harness h;
    h.pulls = 5;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "r", .type = "recorder", .strategy_id = 3}, h.types));
    ASSERT_TRUE(live.start(3, {}));
    ASSERT_TRUE(live.stop(3));
    EXPECT_EQ(live.find(3)->orders_cancelled_on_stop, 5U);
}

TEST(StrategyRegistry, EveryRefusalOfAStartOrAStopIsNamed) {
    Harness h;
    Registry live{h.kernel()};
    EXPECT_EQ(live.start(3, {}).error(), ControlError::NoSuchStrategy);
    EXPECT_EQ(live.stop(3).error(), ControlError::NoSuchStrategy);

    ASSERT_TRUE(live.create({.name = "r", .type = "recorder", .strategy_id = 3}, h.types));
    // A stop before a start: nothing to unwind.
    EXPECT_EQ(live.stop(3).error(), ControlError::NotRunning);
    ASSERT_TRUE(live.start(3, {}));
    EXPECT_EQ(live.start(3, {}).error(), ControlError::AlreadyRunning);
    ASSERT_TRUE(live.stop(3));
    // And from the end, neither move.
    EXPECT_EQ(live.start(3, {}).error(), ControlError::Finished);
    EXPECT_EQ(live.stop(3).error(), ControlError::Finished);

    // A refused start ran no hook and a refused stop pulled no order.
    EXPECT_EQ(journal(), (std::vector<std::string>{"s3:start", "s3:stop", "cancel:3"}));
}

TEST(StrategyRegistry, TheProcessStopSignalTearsDownThroughTheSamePath) {
    // A shutdown must not be a shorter unwind than a person's stop.
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "r", .type = "recorder", .strategy_id = 3}, h.types));
    std::stop_source shutdown;
    ASSERT_TRUE(live.start(3, shutdown.get_token()));

    EXPECT_EQ(live.poll(), 0U) << "nothing asked it to stop";
    shutdown.request_stop();
    EXPECT_EQ(live.poll(), 1U);
    EXPECT_EQ(live.find(3)->phase, Phase::Stopped);
    EXPECT_EQ(journal(), (std::vector<std::string>{"s3:start", "s3:stop", "cancel:3"}));
    // And it does not stop twice.
    EXPECT_EQ(live.poll(), 0U);
}

TEST(StrategyRegistry, AStopDoesNotFlattenAPosition) {
    // The registry is handed exactly one thing to do to the order path, and it
    // is a cancel. Nothing here can reduce a position, and this test is what
    // keeps a later "helpful" flatten from being added quietly.
    Harness h;
    std::vector<std::string> asked;
    auto kernel = h.kernel();
    kernel.cancel_working = [&](std::uint16_t id) -> std::size_t {
        asked.push_back("cancel:" + std::to_string(id));
        return 2;
    };
    Registry live{std::move(kernel)};
    ASSERT_TRUE(live.create({.name = "r", .type = "recorder", .strategy_id = 3}, h.types));
    ASSERT_TRUE(live.start(3, {}));
    ASSERT_TRUE(live.stop(3));
    EXPECT_EQ(asked, (std::vector<std::string>{"cancel:3"}));
    // And the teardown did nothing else at all: no second call, no hook the
    // registry invented, nothing between on_stop and the cancel.
    EXPECT_EQ(journal(), (std::vector<std::string>{"s3:start", "s3:stop"}));
}

// ----------------------------------------------------------- dispatch ----

TEST(StrategyRegistry, MarketDataReachesEveryRunningInstance) {
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "a", .type = "recorder", .strategy_id = 3}, h.types));
    ASSERT_TRUE(live.create({.name = "b", .type = "recorder", .strategy_id = 4}, h.types));
    ASSERT_TRUE(live.start(3, {}));
    ASSERT_TRUE(live.start(4, {}));
    journal().clear();

    live.on_bbo({});
    live.on_trade({});
    live.on_funding({});
    EXPECT_EQ(journal(),
              (std::vector<std::string>{"s3:bbo",
                                        "s4:bbo",
                                        "s3:trade",
                                        "s4:trade",
                                        "s3:funding",
                                        "s4:funding"}));
}

TEST(StrategyRegistry, AnOrderEventReachesOnlyTheStrategyThatPlacedIt) {
    // The client order id packs the strategy that placed the order, so the
    // event names its own owner. Handing it to the others would show a
    // strategy somebody else's fills.
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "a", .type = "recorder", .strategy_id = 3}, h.types));
    ASSERT_TRUE(live.create({.name = "b", .type = "recorder", .strategy_id = 4}, h.types));
    ASSERT_TRUE(live.start(3, {}));
    ASSERT_TRUE(live.start(4, {}));
    journal().clear();

    OrderEvent event{};
    event.client_order_id = ClientOrderId::make(4, 17);
    live.on_order_event(event);
    EXPECT_EQ(journal(), (std::vector<std::string>{"s4:order"}));

    // An id nobody here owns — the manual desk's, or an order from a previous
    // run that reconciliation closed — lands nowhere.
    journal().clear();
    OrderEvent stray{};
    stray.client_order_id = ClientOrderId::make(k_manual_strategy_id, 1);
    live.on_order_event(stray);
    OrderEvent unattributed{};
    live.on_order_event(unattributed);
    EXPECT_TRUE(journal().empty());
}

TEST(StrategyRegistry, ATimerReachesOneStrategyAndNotItsNeighbour) {
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "a", .type = "recorder", .strategy_id = 3}, h.types));
    ASSERT_TRUE(live.create({.name = "b", .type = "recorder", .strategy_id = 4}, h.types));
    ASSERT_TRUE(live.start(3, {}));
    ASSERT_TRUE(live.start(4, {}));
    journal().clear();

    live.on_timer(4, 1);
    live.on_timer(99, 1); // nobody
    EXPECT_EQ(journal(), (std::vector<std::string>{"s4:timer"}));
}

TEST(StrategyRegistry, StoppingOneLeavesTheOtherRunning) {
    Harness h;
    Registry live{h.kernel()};
    ASSERT_TRUE(live.create({.name = "a", .type = "recorder", .strategy_id = 3}, h.types));
    ASSERT_TRUE(live.create({.name = "b", .type = "recorder", .strategy_id = 4}, h.types));
    ASSERT_TRUE(live.start(3, {}));
    ASSERT_TRUE(live.start(4, {}));
    ASSERT_TRUE(live.stop(3));
    journal().clear();

    live.on_bbo({});
    EXPECT_EQ(journal(), (std::vector<std::string>{"s4:bbo"}));
    EXPECT_EQ(live.running(), 1U);
    EXPECT_EQ(live.size(), 2U) << "a stopped strategy stops being listed, so it stops being "
                                  "accounted for";
}

TEST(StrategyRegistry, AListingKeepsCreationOrderAndCarriesWhenEachMoved) {
    Harness h;
    Registry live{h.kernel()};
    h.now_ns = 100;
    ASSERT_TRUE(live.create({.name = "first", .type = "quiet", .strategy_id = 8}, h.types));
    h.now_ns = 200;
    ASSERT_TRUE(live.create({.name = "second", .type = "quiet", .strategy_id = 7}, h.types));
    h.now_ns = 300;
    ASSERT_TRUE(live.start(7, {}));

    const auto listed = live.list();
    ASSERT_EQ(listed.size(), 2U);
    EXPECT_EQ(listed[0].name, "first");
    EXPECT_EQ(listed[1].name, "second");
    EXPECT_EQ(listed[0].since.ns, 100U);
    EXPECT_EQ(listed[1].since.ns, 300U) << "`since` is when it entered its phase, not when it "
                                           "was created";
    EXPECT_EQ(listed[1].phase, Phase::Running);
}

} // namespace
} // namespace raikiri::strategy
