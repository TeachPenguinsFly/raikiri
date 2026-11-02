#include <raikiri/bookrisk/desk.hpp>
#include <raikiri/bookrisk/file_store.hpp>
#include <raikiri/bookrisk/notifiers.hpp>
#include <raikiri/bookrisk/value.hpp>
#include <raikiri/portfolio/position.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "fakes.hpp"

namespace raikiri::bookrisk {
namespace {

using namespace raikiri::bookrisk::test;

class Files : public ::testing::Test {
protected:
    std::filesystem::path root{std::filesystem::temp_directory_path() /
                               ("bookrisk-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                ::testing::UnitTest::GetInstance()->current_test_info()->name())};
    std::string path{(root / "risk" / "settings.json").string()};

    void TearDown() override { std::filesystem::remove_all(root); }
};

TEST_F(Files, NothingSavedIsNotAnError) {
    FileSettingsStore store{path};
    const auto loaded = store.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->has_value());
}

TEST_F(Files, TheDocumentRoundTrips) {
    DeskSettings doc{
        .capital = d("250000.5"),
        .capital_by = "vlad",
        .capital_at = {1'800'000'000'000'000'000},
        .desk = {.state = oms::BookState::Halted, .reason = "a \"quoted\" one", .by = "vlad", .since = {42}}};
    doc.defaults[Param::OrderNotional] = {.mode = LimitMode::Percent, .value = d("1.5")};
    doc.defaults[Param::QuoteAgeMs] = {.mode = LimitMode::Off};
    doc.defaults.loss_action = LossAction::Reducing;
    auto& book = doc.books[7];
    book.limits[Param::OpenOrders] = {.mode = LimitMode::Absolute, .value = d("12")};
    book.state = {.state = oms::BookState::Reducing, .reason = "why", .by = "anna", .since = {7}};
    book.day = {.day = 20'800, .start_pnl = d("-12.25")};

    FileSettingsStore store{path};
    ASSERT_TRUE(store.save(doc).has_value());
    const auto loaded = store.load();
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    ASSERT_TRUE(loaded->has_value());
    const auto& got = **loaded;
    EXPECT_EQ(got.capital, doc.capital);
    EXPECT_EQ(got.capital_by, "vlad");
    EXPECT_EQ(got.capital_at.ns, doc.capital_at.ns);
    EXPECT_EQ(got.defaults, doc.defaults);
    EXPECT_EQ(got.desk.state, oms::BookState::Halted);
    EXPECT_EQ(got.desk.reason, doc.desk.reason);
    ASSERT_EQ(got.books.count(7), 1U);
    EXPECT_EQ(got.books.at(7).limits, book.limits);
    EXPECT_EQ(got.books.at(7).state.state, oms::BookState::Reducing);
    EXPECT_EQ(got.books.at(7).state.by, "anna");
    EXPECT_EQ(got.books.at(7).day.day, 20'800U);
    EXPECT_EQ(got.books.at(7).day.start_pnl, d("-12.25"));
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

TEST_F(Files, ATornFileIsAnErrorSoAHaltIsNeverForgotten) {
    std::filesystem::create_directories(std::filesystem::path{path}.parent_path());
    std::ofstream{path} << R"({"capital":"1000","books":{"7":{"state":{"state":"hal)";
    FileSettingsStore store{path};
    EXPECT_FALSE(store.load().has_value());
}

TEST_F(Files, AnUnknownStateIsAnErrorRatherThanActive) {
    std::filesystem::create_directories(std::filesystem::path{path}.parent_path());
    std::ofstream{path} << R"({"books":{"7":{"state":{"state":"sleeping"}}}})";
    FileSettingsStore store{path};
    EXPECT_FALSE(store.load().has_value());
}

TEST_F(Files, ARestartKeepsHaltedThroughTheRealFile) {
    FakeKernel kernel;
    MemoryAudit audit;
    MemoryNotifier notices;
    TestClock clock{UnixNanos{1'800'000'000'000'000'000}};
    FileSettingsStore store{path};
    {
        RiskDesk desk{kernel, store, audit, notices, clock};
        ASSERT_TRUE(desk.load().has_value());
        ASSERT_TRUE(desk.set_capital(d("1000"), {.by = "vlad", .confirm = true}).has_value());
        ASSERT_TRUE(desk.kill(7, {.by = "vlad", .reason = "stop"}).has_value());
        ASSERT_TRUE(desk.kill(std::nullopt, {.by = "vlad"}).has_value());
    }
    RiskDesk desk{kernel, store, audit, notices, clock};
    ASSERT_TRUE(desk.load().has_value());
    const auto view = desk.view();
    EXPECT_EQ(view.desk.state, oms::BookState::Halted);
    EXPECT_EQ(view.capital, d("1000"));
    const auto book = desk.book_view("7");
    EXPECT_EQ(book->own.state, oms::BookState::Halted);
    EXPECT_EQ(book->own.reason, "stop");
    oms::Exposure exposure{};
    desk.admit(7, buy(1), exposure);
    const auto verdict = desk.gate(7).check(buy(1), exposure);
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error().why, oms::RiskReject::BookHalted);
}

struct Posted {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> rows;
};

WebhookPost into(Posted& posted) {
    return [&posted](std::string_view url, std::string_view body) {
        const std::scoped_lock hold{posted.mutex};
        posted.rows.emplace_back(url, body);
        return true;
    };
}

class EnvSecrets final : public config::SecretProvider {
public:
    [[nodiscard]] std::expected<void, config::SecretError> resolve(const config::SecretRef& ref,
                                                                   config::Secret& out) override {
        if (ref.payload != "RISK_HOOK") {
            return std::unexpected{config::SecretError::NotFound};
        }
        return out.assign("https://hooks.example/abc");
    }
};

TEST(Webhook, TheUrlMustBeAReferenceToACredential) {
    EnvSecrets secrets;
    Posted posted;
    const auto literal = make_webhook("literal:https://hooks.example/abc", secrets, into(posted));
    EXPECT_FALSE(literal.has_value());
    EXPECT_FALSE(make_webhook("env:NOT_THERE", secrets, into(posted)).has_value());
    EXPECT_TRUE(make_webhook("env:RISK_HOOK", secrets, into(posted)).has_value());
}

TEST(Webhook, WarningsGoOutAndSteadyLinesStayInTheLog) {
    EnvSecrets secrets;
    Posted posted;
    {
        auto hook = make_webhook("env:RISK_HOOK", secrets, into(posted));
        ASSERT_TRUE(hook.has_value());
        (*hook)->notify({.level = NoticeLevel::Steady, .kind = "pnl", .subject = "book:7", .text = "fine", .at = {1}});
        (*hook)->notify(
            {.level = NoticeLevel::Warning, .kind = "loss_speed", .subject = "book:7", .text = "moved -5", .at = {2}});
        for (int i = 0; i < 200; ++i) {
            {
                const std::scoped_lock hold{posted.mutex};
                if (!posted.rows.empty()) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    ASSERT_EQ(posted.rows.size(), 1U);
    EXPECT_EQ(posted.rows[0].first, "https://hooks.example/abc");
    EXPECT_NE(posted.rows[0].second.find(R"("kind":"loss_speed")"), std::string::npos) << posted.rows[0].second;
}

TEST(Webhook, AFanOutReachesEverySink) {
    MemoryNotifier one;
    MemoryNotifier two;
    FanOutNotifier fan;
    fan.add(one);
    fan.add(two);
    fan.notify({.kind = "x"});
    EXPECT_EQ(one.count("x"), 1U);
    EXPECT_EQ(two.count("x"), 1U);
}

InstrumentBase base_of_kind(ContractKind kind, std::string_view symbol, std::string_view tick, std::string_view lot) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
    base.local_symbol = *SymbolText::from(symbol);
    base.exchange_symbol = *SymbolText::from(symbol);
    base.tick_size = d(tick);
    base.lot_size = d(lot);
    base.contract_value = d("1");
    base.kind = kind;
    return base;
}

portfolio::Position held(const InstrumentBase& base, Side side, std::int64_t lots, std::string_view price) {
    portfolio::Position position{portfolio::PositionKey{7, base.id}, UnixNanos{1}};
    position.apply_fill(base, side, QtyLots::from_raw(lots), *base.price_to_ticks(d(price)), d("0"), UnixNanos{2});
    return position;
}

TEST(Value, ALinearPositionIsCountedInDollars) {
    const auto base = base_of_kind(ContractKind::Linear, "BTC/USDT", "0.1", "0.001");
    const auto position = held(base, Side::Buy, 1'000, "100");
    PnlReading reading{};
    add_position(reading, PerpInstrument{.base = base}, position, {.mid = *base.price_to_ticks(d("110"))});
    EXPECT_EQ(reading.unrealized, d("10"));
    EXPECT_EQ(reading.positions, 1U);
    EXPECT_EQ(reading.unmarked, 0U);
}

TEST(Value, AnInversePositionIsConvertedAtTheMid) {
    const auto base = base_of_kind(ContractKind::Inverse, "BTC/USD", "0.5", "1");
    const auto position = held(base, Side::Buy, 1'000, "100");
    PnlReading reading{};
    add_position(reading, PerpInstrument{.base = base}, position, {.mid = *base.price_to_ticks(d("125"))});
    // 1000 dollars of contracts: 10 coins bought, 8 coins worth now, 2 coins up at 125
    EXPECT_EQ(reading.unrealized, d("250"));
}

TEST(Value, NoMarkIsCountedAndLeftOut) {
    const auto base = base_of_kind(ContractKind::Linear, "BTC/USDT", "0.1", "0.001");
    const auto position = held(base, Side::Buy, 1'000, "100");
    PnlReading reading{};
    add_position(reading, PerpInstrument{.base = base}, position, {});
    EXPECT_EQ(reading.unmarked, 1U);
    EXPECT_EQ(reading.unrealized, d("0"));
}

TEST(Value, AFlatPositionWithNothingBookedNeedsNoMark) {
    const auto base = base_of_kind(ContractKind::Inverse, "BTC/USD", "0.5", "1");
    portfolio::Position position{portfolio::PositionKey{7, base.id}, UnixNanos{1}};
    PnlReading reading{};
    add_position(reading, PerpInstrument{.base = base}, position, {});
    EXPECT_EQ(reading.unmarked, 0U);
    EXPECT_EQ(reading.positions, 0U);
}

TEST(Value, AQuoteThatIsNoDollarIsLeftOutAndCounted) {
    const auto base = base_of_kind(ContractKind::Linear, "ETH/BTC", "0.0001", "0.001");
    const auto position = held(base, Side::Buy, 1'000, "0.05");
    PnlReading reading{};
    add_position(reading, PerpInstrument{.base = base}, position, {.mid = *base.price_to_ticks(d("0.06"))});
    EXPECT_EQ(reading.unvalued, 1U);
    EXPECT_EQ(reading.unmarked, 0U);
    EXPECT_EQ(reading.unrealized, d("0"));
}

TEST(Value, BalancesCountAUnifiedWalletOncePerVenueAndSayWhatTheyCouldNotPrice) {
    BalanceTally tally;
    const auto usdt = *to_asset_code("USDT");
    const auto btc = *to_asset_code("BTC");
    const auto doge = *to_asset_code("DOGE");
    tally.add_account();
    tally.add(VenueId::Bybit, {.asset = usdt, .wallet = WalletKind::Unified, .total = d("1000")}, FixedE9::undef());
    tally.add(VenueId::Bybit, {.asset = usdt, .wallet = WalletKind::Unified, .total = d("1000")}, FixedE9::undef());
    tally.add(VenueId::Okx, {.asset = usdt, .wallet = WalletKind::Unified, .total = d("500")}, FixedE9::undef());
    tally.add(VenueId::Binance, {.asset = usdt, .wallet = WalletKind::Spot, .total = d("10")}, FixedE9::undef());
    tally.add(VenueId::Binance, {.asset = usdt, .wallet = WalletKind::Futures, .total = d("20")}, FixedE9::undef());
    tally.add(VenueId::Binance, {.asset = btc, .wallet = WalletKind::Spot, .total = d("2")}, d("100"));
    tally.add(VenueId::Binance, {.asset = doge, .wallet = WalletKind::Spot, .total = d("99")}, FixedE9::undef());
    EXPECT_EQ(tally.reading().dollars, d("1730"));
    EXPECT_EQ(tally.reading().unvalued, 1U);
    EXPECT_EQ(tally.reading().accounts, 1U);
}

} // namespace
} // namespace raikiri::bookrisk
