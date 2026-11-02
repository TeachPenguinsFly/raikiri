#pragma once

// {"seq":N,"ts":"N","actor":"..","action":"..","subject":"..","detail":{..},"prev":"hex"}
// the hash covers a line's bytes without its newline, so verifying never has to re-serialise

#include <raikiri/audit/chain.hpp>
#include <raikiri/audit/log.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace raikiri::audit {

// true when `detail_json` went in as given rather than wrapped as {"unparsed":"..."}: not json, or spread over lines
bool append_line(std::string& out, std::uint64_t seq, const Entry& entry, const Digest& prev);

// strings are still escaped; views into `line`
struct ParsedLine {
    std::uint64_t seq{0};
    std::uint64_t ts{0};
    std::string_view actor{};
    std::string_view action{};
    std::string_view subject{};
    std::string_view detail{};
    Digest prev{};
};

// reads the layout append_line writes and no other
[[nodiscard]] std::optional<ParsedLine> parse_line(std::string_view line) noexcept;

void unescape_to(std::string& out, std::string_view escaped);

} // namespace raikiri::audit
