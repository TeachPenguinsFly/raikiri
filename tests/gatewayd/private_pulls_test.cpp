#include <gtest/gtest.h>

#include "private_pulls.hpp"

namespace {

using raikiri::VenueId;
using raikiri::gatewayd::private_pulls_enabled;

TEST(GatewaydPrivatePulls, AGatewayWithoutCredentialsSchedulesNoPrivatePulls) {
    for (const auto venue : {VenueId::Binance, VenueId::Weex, VenueId::Pacifica}) {
        EXPECT_FALSE(private_pulls_enabled(venue, true, false));
        EXPECT_TRUE(private_pulls_enabled(venue, true, true));
        EXPECT_FALSE(private_pulls_enabled(venue, false, true));
    }
}

TEST(GatewaydPrivatePulls, TheSimVenuePullsWithoutKeys) {
    EXPECT_TRUE(private_pulls_enabled(VenueId::Sim, true, false));
    EXPECT_FALSE(private_pulls_enabled(VenueId::Sim, false, false));
}

} // namespace
