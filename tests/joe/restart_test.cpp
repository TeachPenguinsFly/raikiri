#include <raikiri/joe/desk.hpp>
#include <raikiri/joe/parent_journal.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/oms/replay.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

namespace raikiri::joe {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;

FixedE9 d(std::string_view text) {
    return *parse_decimal_e9(text);
}

constexpr InstrumentId k_instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{7});

Listing btc() {
    Listing out{};
    out.symbol = *SymbolText::from("BTC/USDT");
    return out;
}

class Market final : public MarketView {
public:
    std::vector<VenueBook> books(const Listing&, Side side) const override {
        return {{.instrument = k_instrument,
                 .lot = d("0.001"),
                 .levels = {{side == Side::Buy ? d("100") : d("99.98"), d("1")}}}};
    }

    std::optional<Scale> scale(InstrumentId) const override {
        return Scale{.listing = btc(), .tick = d("0.01"), .lot = d("0.001")};
    }
};

class Sink final : public OrderSink {
public:
    bool running() const noexcept override { return true; }

    std::expected<std::uint64_t, SinkError> submit(const ChildOrder& order) noexcept override {
        sent.push_back(order);
        return next_id + sent.size();
    }

    void cancel(std::uint64_t) noexcept override {}

    std::vector<ChildOrder> sent;
    std::uint64_t next_id{1000};
};

class Stepping final : public Clock {
public:
    [[nodiscard]] UnixNanos wall() const noexcept override { return UnixNanos{now}; }

    [[nodiscard]] MonoNanos mono() const noexcept override { return MonoNanos{now}; }

    std::uint64_t now{1'000'000 * k_s};
};

class RestartTest : public testing::Test {
protected:
    RestartTest()
        : dir_{std::filesystem::temp_directory_path() /
               (std::string{"joe_restart_"} + testing::UnitTest::GetInstance()->current_test_info()->name())} {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    ~RestartTest() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] static ParentSpec twap(std::string_view tag = {}) {
        ParentSpec spec{};
        spec.listing = btc();
        spec.qty = d("0.003");
        spec.limit = FixedE9::undef();
        spec.algo = Algo::Twap;
        spec.horizon_ns = 60 * k_s;
        spec.slices = 3;
        spec.client_tag = std::string{tag};
        return spec;
    }

    // a venue's fill as strategyd journals it and the runner hands it on
    void
    venue_fill(Desk& desk, journal::JournalWriter& out, std::uint64_t child, std::int64_t lots, std::int64_t leaves) {
        OrderEvent event{};
        event.type = OrderEventType::Filled;
        event.instrument = k_instrument;
        event.client_order_id = ClientOrderId{child};
        event.exec_id = *ExecId::from("x" + std::to_string(++execs_));
        event.qty = QtyLots::from_raw(lots);
        event.leaves_qty = QtyLots::from_raw(leaves);
        event.price = PriceTicks::from_raw(10'000);
        event.fee = d("0.01");
        event.ts_event = clock_.wall();
        event.ts_init = clock_.wall();
        oms::append_order_event(out, event);
        desk.on_fill(child, k_instrument, event.qty, event.price, event.fee, event.leaves_qty);
    }

    const raikiri::log::Scope logging_{"joe_restart_test"};
    std::filesystem::path dir_;
    Stepping clock_;
    Market market_;
    Sink sink_;
    CurveProfile curves_;
    std::uint64_t execs_{0};
};

TEST_F(RestartTest, ARecordSurvivesItsEncoding) {
    ParentRecord in{.kind = ParentRecord::Kind::Created, .parent = 9, .ts = UnixNanos{5}, .spec = twap("tag-1")};
    in.spec.peg.offset_bp = -2;
    in.spec.venue = VenueId::Okx;
    in.spec.depth_share_bp = 2500;
    in.spec.depth_band_bp = d("15");
    in.child = {
        .id = 4, .instrument = k_instrument, .lot = d("0.001"), .lots = 3, .limit = d("100"), .tif = ChildTif::Gtc};
    in.times = {UnixNanos{7}};
    in.weights = {2};
    in.reason = "why \"quoted\"";
    const auto out = decode_parent_record(encode_parent_record(in));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->parent, 9U);
    EXPECT_EQ(out->spec.client_tag, "tag-1");
    EXPECT_EQ(out->spec.qty, in.spec.qty);
    EXPECT_TRUE(out->spec.limit.is_undef());
    EXPECT_EQ(out->spec.peg.offset_bp, -2);
    EXPECT_EQ(out->spec.venue, VenueId::Okx);
    EXPECT_EQ(out->spec.depth_share_bp, 2500U);
    EXPECT_EQ(out->spec.depth_band_bp, d("15"));
    EXPECT_EQ(out->child.lots, 3);
    EXPECT_EQ(out->child.tif, ChildTif::Gtc);
    EXPECT_EQ(out->times.at(0).ns, 7U);
    EXPECT_EQ(out->reason, in.reason);
}

// the hedge gap: joe used to forget its parents here, so fills after the last poll went unseen
TEST_F(RestartTest, AParentAndItsFillsSurviveAndAgreeWithThePosition) {
    std::uint64_t id = 0;
    std::uint64_t child = 0;
    FixedE9 filled_before{};
    FixedE9 fees_before{};
    {
        auto out = *journal::JournalWriter::open(dir_.string(), "journal");
        JournalParentLog log{out};
        Desk desk{market_, sink_, clock_, curves_};
        desk.set_log(&log);
        id = *desk.start(twap("hedge-1"));
        desk.tick();
        ASSERT_FALSE(sink_.sent.empty());
        child = sink_.next_id + sink_.sent.size();
        ASSERT_NE(desk.find(id)->child(child), nullptr);
        venue_fill(desk, out, child, 1, 0);
        filled_before = desk.find(id)->filled();
        fees_before = desk.find(id)->fees();
        ASSERT_EQ(filled_before, d("0.001"));
    }

    clock_.now += 5 * k_s;
    Desk after{market_, sink_, clock_, curves_};
    ASSERT_GT(restore_desk(after, dir_.string(), "journal", 0), 0U);
    const auto* parent = after.find(id);
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->filled(), filled_before);
    EXPECT_EQ(parent->fees(), fees_before);
    EXPECT_EQ(parent->status(), Status::Paused);
    EXPECT_EQ(after.find_tag("hedge-1"), parent);
    EXPECT_EQ(parent->qty(), d("0.003"));

    // a second start with the same tag is the same parent, and a new one takes a new id
    EXPECT_EQ(*after.start(twap("hedge-1")), id);
    EXPECT_GT(*after.start(twap("other")), id);
}

TEST_F(RestartTest, AFillOwedToAChildStillOutReachesTheParentAfterTheRestart) {
    std::uint64_t id = 0;
    std::uint64_t child = 0;
    {
        auto out = *journal::JournalWriter::open(dir_.string(), "journal");
        JournalParentLog log{out};
        Desk desk{market_, sink_, clock_, curves_};
        desk.set_log(&log);
        auto spec = twap();
        spec.algo = Algo::Pegged;
        spec.limit = d("100");
        spec.horizon_ns = 0;
        spec.venue = VenueId::Binance;
        id = *desk.start(spec);
        desk.tick();
        ASSERT_FALSE(sink_.sent.empty());
        child = sink_.next_id + sink_.sent.size();
    }
    Desk after{market_, sink_, clock_, curves_};
    ASSERT_GT(restore_desk(after, dir_.string(), "journal", 0), 0U);
    const auto* parent = after.find(id);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(parent->live(), nullptr);
    EXPECT_EQ(parent->live()->id, child);
    EXPECT_EQ(parent->working(), parent->live()->qty());
    const auto sent_before = sink_.sent.size();
    after.on_fill(
        child, k_instrument, QtyLots::from_raw(2), PriceTicks::from_raw(10'000), d("0.01"), QtyLots::from_raw(1));
    EXPECT_EQ(parent->filled(), d("0.002"));
    // paused: a fill books, and sends nothing until the venue has agreed
    EXPECT_EQ(sink_.sent.size(), sent_before);
}

TEST_F(RestartTest, WhatWasWorkingGoesOnOnceTheVenueAgrees) {
    std::uint64_t id = 0;
    {
        auto out = *journal::JournalWriter::open(dir_.string(), "journal");
        JournalParentLog log{out};
        Desk desk{market_, sink_, clock_, curves_};
        desk.set_log(&log);
        id = *desk.start(twap());
        desk.tick();
    }
    Desk after{market_, sink_, clock_, curves_};
    (void)restore_desk(after, dir_.string(), "journal", 0);
    const auto sent_before = sink_.sent.size();
    clock_.now += 30 * k_s;
    after.tick();
    EXPECT_EQ(sink_.sent.size(), sent_before);
    EXPECT_EQ(after.resume_restored(), 1U);
    EXPECT_EQ(after.find(id)->status(), Status::Working);
    after.tick();
    EXPECT_GT(sink_.sent.size(), sent_before);
}

TEST_F(RestartTest, AGracefulStopGoesOnToo) {
    std::uint64_t id = 0;
    {
        auto out = *journal::JournalWriter::open(dir_.string(), "journal");
        JournalParentLog log{out};
        Desk desk{market_, sink_, clock_, curves_};
        desk.set_log(&log);
        id = *desk.start(twap());
        desk.tick();
        desk.halt();
    }
    Desk after{market_, sink_, clock_, curves_};
    (void)restore_desk(after, dir_.string(), "journal", 0);
    EXPECT_EQ(after.find(id)->status(), Status::Paused);
    EXPECT_EQ(after.resume_restored(), 1U);
}

TEST_F(RestartTest, WhatAnOperatorPausedStaysPaused) {
    std::uint64_t id = 0;
    {
        auto out = *journal::JournalWriter::open(dir_.string(), "journal");
        JournalParentLog log{out};
        Desk desk{market_, sink_, clock_, curves_};
        desk.set_log(&log);
        id = *desk.start(twap());
        desk.tick();
        ASSERT_TRUE(desk.pause(id).has_value());
    }
    Desk after{market_, sink_, clock_, curves_};
    (void)restore_desk(after, dir_.string(), "journal", 0);
    EXPECT_EQ(after.find(id)->status(), Status::Paused);
    EXPECT_EQ(after.resume_restored(), 0U);
}

TEST_F(RestartTest, AFinishedParentIsStillAnswerable) {
    std::uint64_t id = 0;
    {
        auto out = *journal::JournalWriter::open(dir_.string(), "journal");
        JournalParentLog log{out};
        Desk desk{market_, sink_, clock_, curves_};
        desk.set_log(&log);
        id = *desk.start(twap());
        ASSERT_TRUE(desk.cancel(id).has_value());
    }
    Desk after{market_, sink_, clock_, curves_};
    (void)restore_desk(after, dir_.string(), "journal", 0);
    ASSERT_NE(after.find(id), nullptr);
    EXPECT_EQ(after.find(id)->status(), Status::Cancelled);
    EXPECT_EQ(after.resume_restored(), 0U);
}

TEST_F(RestartTest, IdenticalSlicesCarryDifferentRepeatKeys) {
    Desk desk{market_, sink_, clock_, curves_};
    auto spec = twap();
    spec.horizon_ns = 20 * k_s;
    (void)*desk.start(spec);
    for (int i = 0; i < 12; ++i) {
        clock_.now += 5 * k_s;
        desk.tick();
    }
    ASSERT_GE(sink_.sent.size(), 2U);
    std::vector<std::uint64_t> keys;
    for (const auto& one : sink_.sent) {
        EXPECT_NE(one.repeat_key, 0U);
        keys.push_back(one.repeat_key);
    }
    std::ranges::sort(keys);
    EXPECT_EQ(std::ranges::adjacent_find(keys), keys.end());
}

} // namespace
} // namespace raikiri::joe
