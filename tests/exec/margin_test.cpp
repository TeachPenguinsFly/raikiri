#include <raikiri/exec/margin.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace raikiri::exec {
namespace {

constexpr UnixNanos k_now{1'700'000'000'000'000'000ULL};
const InstrumentId k_btc = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{3});

// holds a state and records each call, as a venue would
class FakeVenue final : public MarginControl {
public:
    std::expected<MarginState, VenueError> margin_state(InstrumentId) override {
        calls.emplace_back("read");
        if (read_error.has_value()) {
            return std::unexpected{*read_error};
        }
        return held;
    }

    std::expected<void, VenueError>
    set_leverage(InstrumentId, std::uint32_t leverage, const MarginState& current) override {
        calls.push_back("leverage " + std::to_string(leverage) + " under " + std::string{to_string(current.mode)});
        if (leverage_error.has_value()) {
            return std::unexpected{*leverage_error};
        }
        held.leverage = leverage;
        return {};
    }

    std::expected<void, VenueError>
    set_margin_mode(InstrumentId, MarginMode mode, const MarginState& current) override {
        calls.push_back("mode " + std::string{to_string(mode)} + " at " + std::to_string(current.leverage));
        if (mode_error.has_value()) {
            return std::unexpected{*mode_error};
        }
        held.mode = mode;
        return {};
    }

    MarginState held{.leverage = 10, .max_leverage = 0, .mode = MarginMode::Cross};
    std::optional<VenueError> read_error;
    std::optional<VenueError> leverage_error;
    std::optional<VenueError> mode_error;
    std::vector<std::string> calls;
};

MarginRequest set(std::uint32_t leverage, MarginMode mode) {
    return {.id = 7, .instrument = k_btc, .ts_created = k_now, .leverage = leverage, .op = MarginOp::Set, .mode = mode};
}

TEST(ServeMargin, AReadAnswersWhatTheVenueHoldsAndSendsNothingElse) {
    FakeVenue venue;
    const auto reply = serve_margin(&venue, {.id = 3, .instrument = k_btc, .ts_created = k_now}, k_now);
    EXPECT_EQ(reply.id, 3U);
    EXPECT_EQ(reply.instrument, k_btc);
    EXPECT_EQ(reply.outcome, MarginOutcome::Done);
    EXPECT_EQ(reply.after, venue.held);
    EXPECT_EQ(venue.calls, std::vector<std::string>{"read"});
}

TEST(ServeMargin, OnlyWhatDiffersIsSentAndTheModeGoesFirst) {
    FakeVenue venue;
    const auto reply = serve_margin(&venue, set(25, MarginMode::Isolated), k_now);
    EXPECT_EQ(reply.outcome, MarginOutcome::Done);
    EXPECT_EQ(reply.before.leverage, 10U);
    EXPECT_EQ(reply.before.mode, MarginMode::Cross);
    EXPECT_EQ(reply.after.leverage, 25U);
    EXPECT_EQ(reply.after.mode, MarginMode::Isolated);
    // the leverage call is told the mode the first call left
    EXPECT_EQ(venue.calls,
              (std::vector<std::string>{"read", "mode isolated at 10", "leverage 25 under isolated", "read"}));
}

TEST(ServeMargin, AskingForWhatIsHeldSendsNoChange) {
    FakeVenue venue;
    const auto reply = serve_margin(&venue, set(10, MarginMode::Cross), k_now);
    EXPECT_EQ(reply.outcome, MarginOutcome::Done);
    EXPECT_EQ(reply.after, reply.before);
    EXPECT_EQ(venue.calls, std::vector<std::string>{"read"});
}

TEST(ServeMargin, LeverageAboveTheKnownMaximumIsRefusedBeforeTheVenue) {
    FakeVenue venue;
    venue.held.max_leverage = 20;
    const auto reply = serve_margin(&venue, set(21, MarginMode::Unknown), k_now);
    EXPECT_EQ(reply.outcome, MarginOutcome::Refused);
    EXPECT_NE(reply.reason().find("maximum of 20"), std::string_view::npos);
    EXPECT_EQ(venue.calls, std::vector<std::string>{"read"});
}

TEST(ServeMargin, AVenueRefusalCarriesItsCodeAndReason) {
    FakeVenue venue;
    venue.mode_error = VenueError::make(
        ErrorClass::Terminal, -4048, "Margin type cannot be changed if there exists position.");
    const auto reply = serve_margin(&venue, set(25, MarginMode::Isolated), k_now);
    EXPECT_EQ(reply.outcome, MarginOutcome::Refused);
    EXPECT_EQ(reply.venue_code, -4048);
    EXPECT_EQ(reply.reason(), "Margin type cannot be changed if there exists position.");
    // the leverage is not attempted after the mode was refused, and the venue is read again
    EXPECT_EQ(venue.calls, (std::vector<std::string>{"read", "mode isolated at 10", "read"}));
    EXPECT_EQ(reply.after, reply.before);
}

TEST(ServeMargin, ARefusedSecondCallReportsTheHalfThatLanded) {
    FakeVenue venue;
    venue.leverage_error = VenueError::make(ErrorClass::Terminal, -4028, "Leverage 125 is not valid");
    const auto reply = serve_margin(&venue, set(125, MarginMode::Isolated), k_now);
    EXPECT_EQ(reply.outcome, MarginOutcome::Refused);
    EXPECT_EQ(reply.before.mode, MarginMode::Cross);
    EXPECT_EQ(reply.after.mode, MarginMode::Isolated);
    EXPECT_EQ(reply.after.leverage, 10U);
}

TEST(ServeMargin, ACallThatDidNotCompleteIsNotARefusal) {
    FakeVenue venue;
    venue.leverage_error = VenueError::make(ErrorClass::Connectivity, 0, "no response before deadline");
    const auto reply = serve_margin(&venue, set(25, MarginMode::Unknown), k_now);
    EXPECT_EQ(reply.outcome, MarginOutcome::Failed);
    EXPECT_EQ(reply.cls, ErrorClass::Connectivity);
}

TEST(ServeMargin, AVenueWithoutTheCallsAnswersUnsupported) {
    const auto none = serve_margin(nullptr, set(25, MarginMode::Unknown), k_now);
    EXPECT_EQ(none.outcome, MarginOutcome::Unsupported);
    EXPECT_FALSE(none.reason().empty());

    FakeVenue venue;
    venue.mode_error = margin_unsupported("one margin mode for the whole account");
    const auto reply = serve_margin(&venue, set(0, MarginMode::Isolated), k_now);
    EXPECT_EQ(reply.outcome, MarginOutcome::Unsupported);
    EXPECT_EQ(reply.venue_code, 0);
    EXPECT_EQ(reply.reason(), "one margin mode for the whole account");
}

TEST(ServeMargin, ARequestNobodyWaitsForNeverReachesTheVenue) {
    FakeVenue venue;
    const UnixNanos late{k_now.ns + k_margin_request_ttl_ns + 1};
    const auto reply = serve_margin(&venue, set(25, MarginMode::Unknown), late);
    EXPECT_EQ(reply.outcome, MarginOutcome::Failed);
    EXPECT_TRUE(venue.calls.empty());
}

} // namespace
} // namespace raikiri::exec
