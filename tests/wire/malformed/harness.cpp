#include "harness.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace raikiri::wire::malformed {

namespace {

constexpr std::size_t k_big = 16 * 1024;
constexpr std::size_t k_retyped = 48;
constexpr std::array<char, 8> k_flips{'"', '}', ']', ',', '\\', '\0', 'x', ':'};

std::string load(std::string_view venue, std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_VENUE_FIXTURES} / venue / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << "missing fixture " << path.string();
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// Exactly as long as the frame, so a read past its end is ASan's to report.
void feed(Parse parse, std::string_view frame) {
    if (frame.empty()) {
        parse({});
        return;
    }
    const auto bytes = std::make_unique<char[]>(frame.size());
    std::memcpy(bytes.get(), frame.data(), frame.size());
    parse({bytes.get(), frame.size()});
}

std::size_t skip_string(std::string_view s, std::size_t i) {
    for (++i; i < s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
        } else if (s[i] == '"') {
            return i + 1;
        }
    }
    return s.size();
}

std::size_t value_end(std::string_view s, std::size_t i) {
    if (s[i] == '"') {
        return skip_string(s, i);
    }
    if (s[i] == '{' || s[i] == '[') {
        std::size_t depth = 0;
        while (i < s.size()) {
            if (s[i] == '"') {
                i = skip_string(s, i);
                continue;
            }
            if (s[i] == '{' || s[i] == '[') {
                ++depth;
            } else if ((s[i] == '}' || s[i] == ']') && --depth == 0) {
                return i + 1;
            }
            ++i;
        }
        return s.size();
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') {
        ++i;
    }
    return i;
}

std::string_view other_type(char first) {
    switch (first) {
    case '"':
        return "7";
    case '{':
        return "[1,2]";
    case '[':
        return R"({"a":1})";
    default:
        return R"("x")";
    }
}

void retype(Parse parse, std::string_view frame) {
    std::size_t done = 0;
    for (std::size_t i = 0; i < frame.size() && done < k_retyped;) {
        if (frame[i] == '"') {
            i = skip_string(frame, i);
            continue;
        }
        if (frame[i] != ':' && frame[i] != '[' && frame[i] != ',') {
            ++i;
            continue;
        }
        std::size_t start = i + 1;
        while (start < frame.size() && frame[start] == ' ') {
            ++start;
        }
        if (start >= frame.size() || frame[start] == ']' || frame[start] == '}') {
            ++i;
            continue;
        }
        const auto end = value_end(frame, start);
        std::string mutant{frame.substr(0, start)};
        mutant += other_type(frame[start]);
        mutant += frame.substr(end);
        feed(parse, mutant);
        ++done;
        ++i;
    }
}

void unterminate(Parse parse, std::string_view frame) {
    std::size_t quotes = 0;
    for (std::size_t i = 0; i < frame.size() && quotes < 16; ++i) {
        if (frame[i] != '"') {
            continue;
        }
        ++quotes;
        std::string mutant{frame};
        const auto end = skip_string(frame, i);
        if (end > i + 1 && end <= frame.size()) {
            mutant.erase(end - 1, 1);
        }
        feed(parse, mutant);
        i = end - 1;
    }
}

void mutate(Parse parse, std::string_view frame) {
    feed(parse, frame);

    const bool big = frame.size() > k_big;
    const std::size_t prefixes = big ? 48 : 400;
    const std::size_t sites = big ? 12 : 160;

    const auto stride = std::max<std::size_t>(1, frame.size() / prefixes);
    for (std::size_t len = 0; len < frame.size(); len += stride) {
        feed(parse, frame.substr(0, len));
    }

    const auto step = std::max<std::size_t>(1, frame.size() / sites);
    for (std::size_t at = 0; at < frame.size(); at += step) {
        std::string mutant{frame};
        for (const char c : k_flips) {
            mutant[at] = c;
            feed(parse, mutant);
        }
    }

    for (std::size_t at = 0; at < frame.size(); at += step) {
        for (const std::string_view extra : {",", "]", "}", "[", ",]", ",}"}) {
            std::string mutant{frame};
            mutant.insert(at, extra);
            feed(parse, mutant);
        }
    }

    unterminate(parse, frame);
    retype(parse, frame);

    feed(parse, std::string(4096, '[') + std::string{frame} + std::string(4096, ']'));
    std::string objects;
    for (int i = 0; i < 2048; ++i) {
        objects += R"({"a":)";
    }
    feed(parse, objects + std::string{frame} + std::string(2048, '}'));
    feed(parse, std::string(100000, '['));
    feed(parse, std::string(100000, '{'));

    for (const std::string_view odd : {"",
                                       " ",
                                       "\n",
                                       "null",
                                       "{}",
                                       "[]",
                                       "[{}]",
                                       R"("x")",
                                       "0",
                                       "-",
                                       R"({")",
                                       R"({"a")",
                                       R"({"a":)",
                                       "[",
                                       "{",
                                       R"(\u0000)",
                                       "\xff\xfe"}) {
        feed(parse, odd);
    }
}

} // namespace

void hammer(std::string_view venue, std::initializer_list<Case> cases) {
    for (const auto& c : cases) {
        SCOPED_TRACE(std::string{venue} + "/" + std::string{c.fixture});
        const auto frame = load(venue, c.fixture);
        ASSERT_FALSE(frame.empty());
        mutate(c.parse, frame);
    }
}

} // namespace raikiri::wire::malformed
