#pragma once

#include <initializer_list>
#include <string_view>

namespace raikiri::wire::malformed {

using Parse = void (*)(std::string_view);

// every fixture the glob matches goes to parse; a fixture no rule matches fails the test
struct Case {
    std::string_view glob;
    Parse parse;
};

// feeds each fixture and damaged copies of it to its parsers; only a crash or a sanitizer report fails
void hammer(std::string_view venue, std::initializer_list<Case> cases);

} // namespace raikiri::wire::malformed
