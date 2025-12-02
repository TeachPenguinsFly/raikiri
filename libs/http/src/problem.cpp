#include <raikiri/http/server.hpp>

#include <glaze/glaze.hpp>

#include <optional>
#include <string>

namespace raikiri::http {

// At namespace scope rather than in an anonymous one: glaze reflects over
// member names, and a type with no linkage has none to reflect.
struct ProblemBody {
    std::string title;
    int status{0};
    std::optional<std::string> detail{};
};

namespace {

// A detail that has no answer is ABSENT, never an empty string: a client
// rendering `detail` unconditionally would show a blank line where there is
// nothing to say.
constexpr glz::opts k_opts{.skip_null_members = true};

} // namespace

Response problem(int status, std::string_view title, std::string_view detail) {
    ProblemBody out{.title = std::string{title}, .status = status};
    if (!detail.empty()) {
        out.detail = std::string{detail};
    }
    std::string body;
    (void)glz::write<k_opts>(out, body);
    return {.status = status, .content_type = "application/problem+json", .body = std::move(body)};
}

} // namespace raikiri::http
