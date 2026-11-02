#pragma once

#include <raikiri/bookrisk/ports.hpp>
#include <raikiri/shm/records.hpp>

#include <span>

namespace raikiri::bookrisk {

struct AccountChannels {
    VenueId venue{};
    const shm::GatewayChannels* channels{nullptr};
};

// what the gateways' last pulls say; a coin is priced at the first two-sided quote against a dollar any gateway has
[[nodiscard]] BalanceReading read_balances(std::span<const AccountChannels> accounts);

} // namespace raikiri::bookrisk
