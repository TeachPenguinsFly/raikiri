#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "touch_seed.hpp"

namespace {

using raikiri::InstrumentId;
using raikiri::MarketType;
using raikiri::SymbolId;
using raikiri::VenueId;
using raikiri::gatewayd::keeps_seed;
using raikiri::gatewayd::TouchSeed;

constexpr std::uint64_t k_start = 1'000'000'000ULL;

InstrumentId id_of(std::uint32_t symbol) {
    return InstrumentId::make(VenueId::Gate, MarketType::Spot, SymbolId{symbol});
}

// what the sink holds per cell: when the socket last quoted it and what the cell shows
struct Cells {
    std::array<std::uint64_t, 4> socket_ns{};
    std::array<int, 4> shown{};

    void socket(std::uint32_t symbol, std::uint64_t ns, int price) {
        socket_ns[symbol] = ns;
        shown[symbol] = price;
    }

    void seed(std::uint32_t symbol, int price) {
        if (keeps_seed(socket_ns[symbol])) {
            shown[symbol] = price;
        }
    }

    [[nodiscard]] bool quoted(InstrumentId id) const { return socket_ns[id.symbol().raw] != 0; }
};

TEST(GatewaydTouchSeed, OnlyMarketsTheSocketHasNotQuotedAreAskedFor) {
    TouchSeed seed{{id_of(0), id_of(1), id_of(2)}};
    Cells cells;
    seed.arm(k_start);
    cells.socket(0, k_start + 1, 100);
    const auto quoted = [&cells](InstrumentId id) { return cells.quoted(id); };
    EXPECT_TRUE(seed.take(k_start + TouchSeed::k_delay_ns - 1, quoted).empty());
    const auto asked = seed.take(k_start + TouchSeed::k_delay_ns, quoted);
    EXPECT_EQ(asked, (std::vector<InstrumentId>{id_of(1), id_of(2)}));
    EXPECT_TRUE(seed.take(k_start + 2 * TouchSeed::k_delay_ns, quoted).empty()); // one pass per arm
}

TEST(GatewaydTouchSeed, ASocketQuoteIsNeverOverwrittenByALateSeed) {
    Cells cells;
    cells.socket(0, k_start, 100);
    cells.socket(1, k_start + 5, 200); // arrived while the rest read was in flight
    for (const std::uint32_t symbol : {0U, 1U, 2U}) {
        cells.seed(symbol, 999);
    }
    EXPECT_EQ(cells.shown[0], 100);
    EXPECT_EQ(cells.shown[1], 200);
    EXPECT_EQ(cells.shown[2], 999);
    cells.socket(2, k_start + 9, 300); // the socket replaces the seed
    cells.seed(2, 999);
    EXPECT_EQ(cells.shown[2], 300);
}

TEST(GatewaydTouchSeed, AReopenArmsAnotherPassAndARefusalRetriesSoon) {
    TouchSeed seed{{id_of(0)}};
    const auto none = [](InstrumentId) { return false; };
    seed.reopens(0, k_start);
    EXPECT_TRUE(seed.take(k_start + TouchSeed::k_delay_ns, none).empty()); // never armed
    seed.reopens(2, k_start);
    seed.reopens(2, k_start + TouchSeed::k_delay_ns - 1); // no new reopen, no new pass
    EXPECT_EQ(seed.take(k_start + TouchSeed::k_delay_ns, none).size(), 1U);
    seed.retry(k_start);
    EXPECT_EQ(seed.take(k_start + TouchSeed::k_retry_ns, none).size(), 1U);
}

} // namespace
