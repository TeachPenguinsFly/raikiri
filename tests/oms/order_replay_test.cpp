#include <raikiri/journal/writer.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/replay.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <string>

namespace raikiri::oms {
namespace {

using E = OrderEventType;

constexpr InstrumentId k_instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
constexpr UnixNanos k_now{1'000'000'000'000'000'000ULL};

class OrderReplayTest : public testing::Test {
protected:
    OrderReplayTest()
        : dir_{std::filesystem::temp_directory_path() /
               (std::string{"order_replay_"} + std::to_string(::getpid()) + "_" +
                testing::UnitTest::GetInstance()->current_test_info()->name())} {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    ~OrderReplayTest() override { std::filesystem::remove_all(dir_); }

    OrderCmd create(std::uint64_t seq, std::int64_t lots = 10) {
        OrderCmd cmd{};
        cmd.instrument = k_instrument;
        cmd.client_order_id = ClientOrderId::make(7, seq);
        cmd.exchange_client_order_id = ids_.next_exchange_client_id(k_now);
        cmd.price = PriceTicks::from_raw(100);
        cmd.qty = QtyLots::from_raw(lots);
        cmd.ts_created = k_now;
        return cmd;
    }

    static OrderEvent attributed(const OrderCmd& cmd, E type) {
        OrderEvent out{};
        out.type = type;
        out.instrument = cmd.instrument;
        out.client_order_id = cmd.client_order_id;
        out.exchange_client_order_id = cmd.exchange_client_order_id;
        out.exchange_order_id = *ExchangeOrderId::from("V" + std::to_string(cmd.client_order_id.seq()));
        out.ts_event = k_now;
        out.ts_init = k_now;
        out.qty = QtyLots::undef();
        out.leaves_qty = QtyLots::undef();
        out.price = PriceTicks::undef();
        return out;
    }

    static OrderEvent fill(const OrderCmd& cmd, std::string_view exec, std::int64_t lots, std::int64_t leaves) {
        auto out = attributed(cmd, E::Filled);
        out.exec_id = *ExecId::from(exec);
        out.qty = QtyLots::from_raw(lots);
        out.leaves_qty = QtyLots::from_raw(leaves);
        out.price = PriceTicks::from_raw(100);
        out.fee = FixedE9::from_raw(0);
        return out;
    }

    journal::JournalWriter writer() { return *journal::JournalWriter::open(dir_.string(), "journal"); }

    void rebuild() { replay_orders(dir_.string(), "journal", replay_); }

    std::filesystem::path dir_;
    IdGen ids_{7, 9};
    OrderStore store_{16};
    ExecPath path_{store_};
    OrderReplay replay_{store_, path_};
};

TEST_F(OrderReplayTest, AnOrderOpenBeforeTheCrashIsOpenAfterIt) {
    const auto cmd = create(1);
    {
        auto out = writer();
        append_order_cmd(out, cmd);
        append_order_event(out, attributed(cmd, E::Accepted));
        append_order_event(out, fill(cmd, "x1", 3, 7));
    }
    rebuild();
    const auto* order = store_.find(cmd.client_order_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->status(), OrderStatus::PartiallyFilled);
    EXPECT_EQ(order->leaves().raw, 7);
    EXPECT_EQ(order->exchange_client_order_id(), cmd.exchange_client_order_id);
    EXPECT_EQ(order->price().raw, 100);
    EXPECT_EQ(store_.find_by_exchange_id(*ExchangeOrderId::from("V1")), order);
}

TEST_F(OrderReplayTest, AnExecutionBookedBeforeTheCrashIsNotBookedAgainByTheRing) {
    const auto cmd = create(1);
    {
        auto out = writer();
        append_order_cmd(out, cmd);
        append_order_event(out, fill(cmd, "x1", 3, 7));
    }
    rebuild();
    auto again = fill(cmd, "x1", 3, 7);
    EXPECT_TRUE(path_.apply(again, false, k_now).repeat);
    auto missed = fill(cmd, "x2", 2, 5);
    EXPECT_EQ(path_.apply(missed, false, k_now).delta.raw, 2);
}

TEST_F(OrderReplayTest, AFillWithNoExecIdIsNotBookedAgainEither) {
    const auto cmd = create(1);
    auto first = fill(cmd, "x1", 3, 7);
    first.exec_id = {};
    {
        auto out = writer();
        append_order_cmd(out, cmd);
        append_order_event(out, first);
    }
    rebuild();
    auto again = first;
    EXPECT_EQ(path_.apply(again, false, k_now).delta.raw, 0);
}

TEST_F(OrderReplayTest, WhatClosedBeforeTheCrashStaysClosed) {
    const auto gone = create(1);
    const auto recon = create(2);
    const auto never_sent = create(3);
    {
        auto out = writer();
        append_order_cmd(out, gone);
        append_order_event(out, attributed(gone, E::Canceled));
        append_order_cmd(out, recon);
        append_order_event(out, attributed(recon, E::ReconClosed));
        append_order_cmd(out, never_sent);
        append_order_event(out, attributed(never_sent, E::Denied));
    }
    rebuild();
    EXPECT_EQ(store_.open_count(), 0U);
}

TEST_F(OrderReplayTest, ACancelInFlightStaysCancelling) {
    const auto cmd = create(1);
    auto cancel = cmd;
    cancel.kind = CmdKind::Cancel;
    {
        auto out = writer();
        append_order_cmd(out, cmd);
        append_order_cmd(out, cancel);
    }
    rebuild();
    const auto* order = store_.find(cmd.client_order_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->status(), OrderStatus::PendingCancel);
}

TEST_F(OrderReplayTest, IdsPickUpPastTheJournal) {
    {
        auto out = writer();
        append_order_cmd(out, create(1));
        append_order_cmd(out, create(41));
    }
    rebuild();
    EXPECT_EQ(replay_.last_seq(7), 41U);
    EXPECT_EQ(replay_.last_seq(8), 0U);
}

TEST_F(OrderReplayTest, ALateFillForAnOrderClosedBeforeTheCrashStillFindsItsOwner) {
    const auto cmd = create(1);
    {
        auto out = writer();
        append_order_cmd(out, cmd);
        append_order_event(out, attributed(cmd, E::Accepted));
        append_order_event(out, attributed(cmd, E::Canceled));
    }
    rebuild();
    auto late = fill(cmd, "x9", 4, 6);
    late.client_order_id = {};
    const auto done = path_.apply(late, false, k_now);
    EXPECT_TRUE(done.late);
    EXPECT_EQ(done.delta.raw, 4);
    EXPECT_EQ(late.client_order_id, cmd.client_order_id);
}

TEST_F(OrderReplayTest, NoJournalIsAFirstRun) {
    std::filesystem::remove_all(dir_);
    rebuild();
    EXPECT_EQ(replay_.commands(), 0U);
}

} // namespace
} // namespace raikiri::oms
