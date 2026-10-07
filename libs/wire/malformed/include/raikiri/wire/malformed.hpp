#pragma once

#include <filesystem>
#include <initializer_list>
#include <string_view>

namespace raikiri::wire::malformed {

using Parse = void (*)(std::string_view);

// every fixture the glob matches goes to parse; a fixture no rule matches fails the test
struct Case {
    std::string_view glob{};
    Parse parse{nullptr};
};

// feeds each fixture in dir and damaged copies of it to its parsers; only a crash or a sanitizer report fails
void hammer(std::string_view label, const std::filesystem::path& dir, std::initializer_list<Case> cases);

// the same damage applied to one frame held in memory
void hammer_frame(std::string_view label, std::string_view frame, std::initializer_list<Parse> parsers);

} // namespace raikiri::wire::malformed
