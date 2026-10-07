#pragma once

#include <raikiri/wire/malformed.hpp>

#include <filesystem>
#include <initializer_list>
#include <string_view>

namespace raikiri::wire::malformed {

inline void hammer(std::string_view venue, std::initializer_list<Case> cases) {
    hammer(venue, std::filesystem::path{RAIKIRI_VENUE_FIXTURES} / venue / "fixtures", cases);
}

} // namespace raikiri::wire::malformed
