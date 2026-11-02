#include <raikiri/optiondata/shm_market.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <string>

using namespace raikiri;
using namespace raikiri::optiondata;

namespace {

constexpr UnixNanos k_expiry{1'796'198'400'000'000'000ULL};

std::string segment_dir() {
    return "/tmp/raikiri-optiondata-" + std::to_string(::getpid());
}

struct SegmentDir {
    SegmentDir() {
        std::error_code ec;
        std::filesystem::remove_all(segment_dir(), ec);
        std::filesystem::create_directories(segment_dir(), ec);
    }

    SegmentDir(const SegmentDir&) = delete;
    SegmentDir& operator=(const SegmentDir&) = delete;

    ~SegmentDir() {
        std::error_code ec;
        std::filesystem::remove_all(segment_dir(), ec);
    }
};

FixedE9 e9(const char* text) {
    return *parse_decimal_e9(text);
}

// the catalogue assigns slots in push order, so a SymbolId is its slot
Instrument option(std::uint32_t slot, const char* strike, PutCall pc) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{slot});
    base.exchange_symbol = *SymbolText::from("BTC-2DEC26");
    base.underlying = *UnderlyingText::from("BTC");
    base.tick_size = e9("0.0001");
    base.lot_size = e9("0.1");
    base.contract_value = e9("1");
    base.kind = ContractKind::Inverse;
    return Instrument{OptionInstrument{.base = base,
                                       .strike = *base.price_to_ticks(e9(strike)),
                                       .expiry = k_expiry,
                                       .put_call = pc,
                                       .premium = OptionUnit::Base,
                                       .greeks = OptionUnit::Base}};
}

} // namespace

TEST(ShmOptionMarket, ReadsListingsBooksAndGreeksAndFollowsAGrowingCatalogue) {
    const SegmentDir dir;
    auto owner = shm::SegmentOwner::create(segment_dir() + "/gw.deribit", shm::k_gateway_segment_bytes, 1, 1);
    ASSERT_TRUE(owner.has_value());
    const auto channels = shm::create_gateway_channels(*owner);
    ASSERT_TRUE(channels.has_value());
    ASSERT_TRUE(channels->catalogue->push(option(0, "100000", PutCall::Call)).has_value());
    ASSERT_TRUE(channels->catalogue->push(option(1, "100000", PutCall::Put)).has_value());
    const auto call = base_of(
        *channels->catalogue->find(InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{0})));
    channels->bbo->write(0,
                         Bbo{.instrument = call.id,
                             .bid = *call.price_to_ticks(e9("0.05")),
                             .ask = *call.price_to_ticks(e9("0.0505")),
                             .bid_qty = *call.qty_to_lots(e9("1.5")),
                             .ask_qty = *call.qty_to_lots(e9("2")),
                             .ts_event = UnixNanos{7}});
    OptionGreeks greeks{};
    greeks.instrument = call.id;
    greeks.delta = e9("0.52");
    greeks.mark_iv = e9("0.5");
    greeks.index_price = e9("100000");
    channels->greeks->write(0, greeks);
    channels->ticker_stats->write(0, TickerStats{.instrument = call.id, .open_interest = e9("12")});
    owner->publish();

    admin::Registry registry;
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    const ShmOptionMarket market{registry};

    auto all = market.underlyings();
    ASSERT_EQ(all.size(), 1U);
    EXPECT_EQ(all[0].underlying.view(), "BTC");
    ASSERT_EQ(all[0].expiries.size(), 1U);
    EXPECT_EQ(all[0].expiries[0].expiry, k_expiry);
    EXPECT_EQ(all[0].expiries[0].strikes, 1U);

    std::vector<OptionQuote> quotes;
    market.read("BTC", k_expiry, quotes);
    ASSERT_EQ(quotes.size(), 2U);
    const auto& c = quotes[0];
    EXPECT_EQ(c.key.put_call, PutCall::Call);
    EXPECT_EQ(c.key.strike, e9("100000"));
    EXPECT_EQ(c.bid, e9("0.05"));
    EXPECT_EQ(c.ask, e9("0.0505"));
    EXPECT_EQ(c.bid_size, e9("1.5"));
    ASSERT_TRUE(c.greeks.has_value());
    EXPECT_EQ(c.greeks->delta, e9("0.52"));
    EXPECT_EQ(c.open_interest, e9("12"));
    EXPECT_EQ(c.premium, OptionUnit::Base);
    EXPECT_TRUE(quotes[1].bid.is_undef()) << "an unwritten cell is no quote";
    EXPECT_FALSE(quotes[1].greeks.has_value());

    market.read("ETH", std::nullopt, quotes);
    EXPECT_TRUE(quotes.empty());

    ASSERT_TRUE(channels->catalogue->push(option(2, "110000", PutCall::Call)).has_value());
    all = market.underlyings();
    EXPECT_EQ(all[0].expiries[0].strikes, 2U);
    market.read("BTC", std::nullopt, quotes);
    EXPECT_EQ(quotes.size(), 3U);
}
