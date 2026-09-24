#pragma once

#include <initializer_list>
#include <string_view>

namespace raikiri::wire::malformed {

using Parse = void (*)(std::string_view);

struct Case {
    std::string_view fixture;
    Parse parse;
};

// feeds each fixture and damaged copies of it to its parser; only a crash fails
void hammer(std::string_view venue, std::initializer_list<Case> cases);

} // namespace raikiri::wire::malformed
