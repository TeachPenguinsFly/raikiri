#include <raikiri/oms/funding_identity.hpp>

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>

namespace raikiri::oms {
namespace {

constexpr InstrumentId k_btc = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});
constexpr InstrumentId k_eth = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{2});

std::string identity_of(InstrumentId instrument, UnixNanos at, std::uint16_t strategy) {
    std::array<char, k_max_funding_identity> buf{};
    return std::string{funding_identity(instrument, at, strategy, buf)};
}

TEST(FundingIdentity, TheLongestPossibleKeyStillFitsTheJournalField) {
    // THE BUG THIS EXISTS FOR. The field holds 40 bytes and sbepp's
    // assign_range is an unbounded copy behind a debug-only assert, so the
    // previous key — symbol text, second, strategy — reached 41 on a
    // 24-character symbol and lost its last byte, which is a strategy id
    // digit. Two strategies then shared one key and the ledger's
    // ON CONFLICT DO NOTHING kept only the first.
    constexpr std::size_t k_journal_field = 40;
    const auto widest = identity_of(InstrumentId{std::numeric_limits<std::uint64_t>::max()},
                                    UnixNanos{std::numeric_limits<std::uint64_t>::max()},
                                    std::numeric_limits<std::uint16_t>::max());
    EXPECT_FALSE(widest.empty());
    EXPECT_LE(widest.size(), k_journal_field) << "would be truncated into the amount beside it";
    EXPECT_LE(widest.size(), k_max_funding_identity);
}

TEST(FundingIdentity, TwoStrategiesOnOneSettlementAreToldApart) {
    const UnixNanos at{1'788'696'006'115'000'000ULL};
    EXPECT_NE(identity_of(k_btc, at, 1), identity_of(k_btc, at, 10))
        << "a truncated digit is exactly how one strategy's funding vanished";
    EXPECT_NE(identity_of(k_btc, at, 9'999), identity_of(k_btc, at, 10'000));
}

TEST(FundingIdentity, TwoInstrumentsSettlingInTheSameSecondAreToldApart) {
    const UnixNanos at{1'788'696'006'115'000'000ULL};
    EXPECT_NE(identity_of(k_btc, at, 1), identity_of(k_eth, at, 1));
}

TEST(FundingIdentity, TheSameSettlementStatedAtTwoResolutionsIsOneKey) {
    // The reason it is seconds: the private socket and the REST ledger state
    // the stamp differently, and keying on nanoseconds would book the same
    // money twice.
    const UnixNanos socket{1'788'696'006'115'000'000ULL};
    const UnixNanos ledger{1'788'696'006'000'000'000ULL};
    EXPECT_EQ(identity_of(k_btc, socket, 3), identity_of(k_btc, ledger, 3));
}

TEST(FundingIdentity, ConsecutiveSettlementsAreNotOneKey) {
    const UnixNanos first{1'788'696'006'000'000'000ULL};
    const UnixNanos second{1'788'696'007'000'000'000ULL};
    EXPECT_NE(identity_of(k_btc, first, 3), identity_of(k_btc, second, 3));
}

TEST(FundingIdentity, ABufferTooSmallYieldsNothingRatherThanHalfAKey) {
    std::array<char, k_max_funding_identity - 1> cramped{};
    EXPECT_TRUE(funding_identity(k_btc, UnixNanos{1'000'000'000}, 1, cramped).empty());
}

} // namespace
} // namespace raikiri::oms
