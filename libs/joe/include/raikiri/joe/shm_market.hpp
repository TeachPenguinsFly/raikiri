#pragma once

// books, scales and fees from the gateways' shared memory; only venues whose order entry is wired

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/joe/ports.hpp>
#include <raikiri/shm/records.hpp>

#include <map>
#include <string>
#include <vector>

namespace raikiri::joe {

struct GatewayView {
    AccountId account{};
    shm::GatewayChannels channels{};
};

class ShmMarket final : public MarketView {
public:
    ShmMarket(std::vector<GatewayView> gateways, const Clock& clock);

    [[nodiscard]] std::vector<VenueBook> books(const Listing& listing, Side side) const override;
    [[nodiscard]] std::optional<Scale> scale(InstrumentId instrument) const override;

private:
    struct Found {
        std::vector<InstrumentId> ids;
        std::uint64_t at{0};
    };

    [[nodiscard]] const GatewayView* gateway_for(InstrumentId id) const noexcept;
    [[nodiscard]] const std::vector<InstrumentId>& listed(const Listing& listing) const;

    std::vector<GatewayView> gateways_;
    const Clock* clock_;
    mutable std::map<std::string, Found, std::less<>> listed_;
};

} // namespace raikiri::joe
