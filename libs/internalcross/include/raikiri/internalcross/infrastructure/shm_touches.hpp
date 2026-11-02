#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/internalcross/domain/match.hpp>
#include <raikiri/shm/records.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace raikiri::internalcross {

struct TouchGateway {
    AccountId account{};
    shm::GatewayChannels channels{};
};

// each venue's best bid and ask from the gateways' shared memory; siblings of a listing are looked up once a minute
class ShmTouches {
public:
    ShmTouches(std::vector<TouchGateway> gateways, const Clock& clock);

    [[nodiscard]] std::size_t read(InstrumentId instrument, std::span<Touch> out);

private:
    struct Sibling {
        const TouchGateway* gateway{nullptr};
        InstrumentId id{};
        FixedE9 tick{};
    };

    struct Found {
        std::vector<Sibling> siblings;
        std::uint64_t at{0};
    };

    void relist(InstrumentId instrument, Found& found) const;

    std::vector<TouchGateway> gateways_;
    const Clock* clock_;
    std::unordered_map<std::uint64_t, Found> listed_;
};

} // namespace raikiri::internalcross
