#pragma once

#include <initializer_list>
#include <string_view>

namespace raikiri::wire::malformed {

using Parse = void (*)(std::string_view);

struct Case {
    std::string_view fixture;
    Parse parse;
};

// Feeds each fixture, and some hundreds of damaged copies of it, to its
// parser. Refusing or accepting is fine; only a crash fails, which is why the
// test means something under ASan and UBSan.
void hammer(std::string_view venue, std::initializer_list<Case> cases);

} // namespace raikiri::wire::malformed
