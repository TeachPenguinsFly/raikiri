#include "harness.hpp"

#include <gtest/gtest.h>

#include <fnmatch.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define RAIKIRI_HAS_SANITIZER_HOOK 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RAIKIRI_HAS_SANITIZER_HOOK 1
#endif
#endif

#ifdef RAIKIRI_HAS_SANITIZER_HOOK
#include <sanitizer/common_interface_defs.h>
#endif

#ifdef RAIKIRI_HAS_SANITIZER_HOOK
// ubsan only prints by default, which would let a report pass
extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}
#endif

namespace raikiri::wire::malformed {

namespace {

constexpr std::uint64_t k_seed = 0x5241494b4952490aULL;
constexpr std::size_t k_full_size = 512;
constexpr std::size_t k_long_string = 4 * 1024;
constexpr std::size_t k_huge_string = 64 * 1024;
constexpr std::array<char, 12> k_flips{'"', '}', ']', ',', '\\', '\0', 'x', ':', '{', '[', '-', '9'};

std::string t_fixture;
std::string_view t_frame;

#ifdef RAIKIRI_HAS_SANITIZER_HOOK
void dump_frame() {
    std::fprintf(stderr, "\nmalformed: fixture %s, frame of %zu bytes:\n", t_fixture.c_str(), t_frame.size());
    std::fwrite(t_frame.data(), 1, std::min<std::size_t>(t_frame.size(), 4096), stderr);
    std::fputc('\n', stderr);
}
#endif

// splitmix64, so the mutants are the same on every platform and standard library
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_{seed} {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }

    std::size_t below(std::size_t n) noexcept { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }

private:
    std::uint64_t state_;
};

std::uint64_t fnv1a(std::string_view s) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : s) {
        h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
    }
    return h;
}

std::size_t scaled(std::size_t n, std::string_view frame) noexcept {
    return std::clamp<std::size_t>(n * k_full_size / std::max<std::size_t>(frame.size(), 1), 2, n);
}

std::string load(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << "missing fixture " << path.string();
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

class Feeder {
public:
    explicit Feeder(std::vector<Parse> parsers) : parsers_{std::move(parsers)} {}

    // exactly as long as the frame so asan catches overreads
    void operator()(std::string_view frame) const {
        const auto bytes = std::make_unique<char[]>(std::max<std::size_t>(frame.size(), 1));
        std::memcpy(bytes.get(), frame.data(), frame.size());
        const std::string_view copy{frame.empty() ? nullptr : bytes.get(), frame.size()};
        t_frame = copy;
        for (const auto parse : parsers_) {
            try {
                parse(copy);
            } catch (const std::exception& e) {
                ADD_FAILURE() << "exception " << e.what() << " on " << std::string{frame.substr(0, 512)};
            } catch (...) {
                ADD_FAILURE() << "exception on " << std::string{frame.substr(0, 512)};
            }
        }
    }

private:
    std::vector<Parse> parsers_;
};

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

struct Spots {
    std::vector<std::size_t> values;
    std::vector<std::size_t> closers;
    std::vector<std::size_t> openers;
    std::vector<std::size_t> quotes;
};

Spots scan(std::string_view frame) {
    Spots out;
    for (std::size_t i = 0; i < frame.size();) {
        const char c = frame[i];
        if (c == '"') {
            out.quotes.push_back(i);
            i = skip_string(frame, i);
            continue;
        }
        if (c == '}' || c == ']') {
            out.closers.push_back(i);
        }
        if (c == '{' || c == '[') {
            out.openers.push_back(i);
        }
        if (c == ':' || c == '[' || c == ',') {
            std::size_t start = i + 1;
            while (start < frame.size() && frame[start] == ' ') {
                ++start;
            }
            if (start < frame.size() && frame[start] != ']' && frame[start] != '}') {
                out.values.push_back(start);
            }
        }
        ++i;
    }
    return out;
}

std::vector<std::size_t> spread(const std::vector<std::size_t>& spots, std::size_t n) {
    if (spots.size() <= n) {
        return spots;
    }
    std::vector<std::size_t> out;
    out.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        out.push_back(spots[k * spots.size() / n]);
    }
    return out;
}

std::string replaced(std::string_view frame, std::size_t from, std::size_t to, std::string_view with) {
    std::string out{frame.substr(0, from)};
    out += with;
    out += frame.substr(to);
    return out;
}

void truncate(const Feeder& feed, std::string_view frame) {
    if (frame.size() <= 96) {
        for (std::size_t len = 0; len < frame.size(); ++len) {
            feed(frame.substr(0, len));
        }
        return;
    }
    const auto cuts = scaled(80, frame);
    for (std::size_t k = 0; k < cuts; ++k) {
        feed(frame.substr(0, k * frame.size() / cuts));
    }
    for (std::size_t len = frame.size() - 16; len < frame.size(); ++len) {
        feed(frame.substr(0, len));
    }
}

void flip(const Feeder& feed, std::string_view frame, Rng& rng) {
    const auto flips = scaled(128, frame);
    std::string mutant{frame};
    for (std::size_t k = 0; k < flips; ++k) {
        const auto at = rng.below(frame.size());
        const auto pick = rng.below(k_flips.size() + 1);
        mutant[at] = pick < k_flips.size() ? k_flips[pick] : static_cast<char>(rng.next());
        feed(mutant);
        mutant[at] = frame[at];
    }
}

void unbalance(const Feeder& feed, std::string_view frame, const Spots& spots) {
    for (const auto at : spread(spots.closers, scaled(16, frame))) {
        std::string mutant{frame};
        mutant.erase(at, 1);
        feed(mutant);
    }
    for (const auto at : spread(spots.openers, scaled(4, frame))) {
        std::string mutant{frame};
        mutant.erase(at, 1);
        feed(mutant);
    }
    for (const auto at : spread(spots.values, scaled(12, frame))) {
        for (const std::string_view extra : {",", "]", "}", "[", ",]", ",}"}) {
            std::string mutant{frame};
            mutant.insert(at, extra);
            feed(mutant);
        }
    }
    for (const auto at : spread(spots.quotes, scaled(8, frame))) {
        const auto end = skip_string(frame, at);
        if (end > at + 1 && end <= frame.size()) {
            std::string mutant{frame};
            mutant.erase(end - 1, 1);
            feed(mutant);
        }
    }
}

void retype(const Feeder& feed, std::string_view frame, const Spots& spots) {
    const std::string long_text = '"' + std::string(k_long_string, '7') + '"';
    const std::array<std::string_view, 14> others{"7",
                                                  "-1",
                                                  "0.5",
                                                  "1e999",
                                                  "18446744073709551616",
                                                  R"("x")",
                                                  R"("")",
                                                  R"("-")",
                                                  long_text,
                                                  "null",
                                                  "true",
                                                  "{}",
                                                  R"({"a":1})",
                                                  "[1,2]"};
    for (const auto at : spread(spots.values, scaled(16, frame))) {
        const auto end = value_end(frame, at);
        for (const auto other : others) {
            feed(replaced(frame, at, end, other));
        }
    }
}

std::vector<std::string_view> members(std::string_view frame, std::size_t open) {
    std::vector<std::string_view> out;
    std::size_t depth = 0;
    std::size_t begin = open + 1;
    for (std::size_t i = open + 1; i < frame.size();) {
        const char c = frame[i];
        if (c == '"') {
            i = skip_string(frame, i);
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
        } else if ((c == '}' || c == ']') && depth-- == 0) {
            if (i > begin) {
                out.push_back(frame.substr(begin, i - begin));
            }
            return out;
        } else if (c == ',' && depth == 0) {
            out.push_back(frame.substr(begin, i - begin));
            begin = i + 1;
        }
        ++i;
    }
    return {};
}

std::string joined(const std::vector<std::string_view>& parts) {
    std::string out{"{"};
    for (std::size_t k = 0; k < parts.size(); ++k) {
        out += k == 0 ? "" : ",";
        out += parts[k];
    }
    return out + "}";
}

// valid json in an unexpected order sends lookups down the wraparound path
void reorder(const Feeder& feed, std::string_view frame, const Spots& spots) {
    for (const auto at : spread(spots.openers, scaled(8, frame))) {
        if (frame[at] != '{') {
            continue;
        }
        auto parts = members(frame, at);
        if (parts.size() < 2) {
            continue;
        }
        const auto end = value_end(frame, at);
        std::ranges::reverse(parts);
        const auto reversed = replaced(frame, at, end, joined(parts));
        feed(reversed);
        feed(std::string_view{reversed}.substr(0, at + (end - at) * 3 / 4));
        auto doubled = parts;
        doubled.push_back(parts.front());
        feed(replaced(frame, at, end, joined(doubled)));
        parts.pop_back();
        feed(replaced(frame, at, end, joined(parts)));
        parts.erase(parts.begin());
        feed(replaced(frame, at, end, joined(parts)));
    }
}

void nest(const Feeder& feed, std::string_view frame) {
    feed(std::string(4096, '[') + std::string{frame} + std::string(4096, ']'));
    std::string objects;
    for (int i = 0; i < 2048; ++i) {
        objects += R"({"a":)";
    }
    feed(objects + std::string{frame} + std::string(2048, '}'));
}

void mutate(const Feeder& feed, std::string_view frame, std::uint64_t seed) {
    Rng rng{seed};
    const auto spots = scan(frame);
    feed(frame);
    truncate(feed, frame);
    flip(feed, frame, rng);
    unbalance(feed, frame, spots);
    retype(feed, frame, spots);
    reorder(feed, frame, spots);
    nest(feed, frame);
}

void odd_frames(const Feeder& feed) {
    feed(std::string(100000, '['));
    feed(std::string(100000, '{'));
    feed('"' + std::string(k_huge_string, 'x'));
    for (const std::string_view odd : {"",
                                       " ",
                                       "\n",
                                       "null",
                                       "{}",
                                       "[]",
                                       "[{}]",
                                       "[[]]",
                                       R"("x")",
                                       "0",
                                       "-",
                                       R"({")",
                                       R"({"a")",
                                       R"({"a":)",
                                       R"({"a":1,})",
                                       R"({"data":null})",
                                       R"({"data":[null]})",
                                       R"({"result":{}})",
                                       "[",
                                       "{",
                                       R"(\u0000)",
                                       "\xff\xfe"}) {
        feed(odd);
    }
}

} // namespace

void hammer(std::string_view venue, std::initializer_list<Case> cases) {
#ifdef RAIKIRI_HAS_SANITIZER_HOOK
    __sanitizer_set_death_callback(dump_frame);
#endif
    const auto dir = std::filesystem::path{RAIKIRI_VENUE_FIXTURES} / venue / "fixtures";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.is_regular_file() && entry.path().filename() != "README.md") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<Parse> all;
    for (const auto& c : cases) {
        if (std::find(all.begin(), all.end(), c.parse) == all.end()) {
            all.push_back(c.parse);
        }
    }
    odd_frames(Feeder{all});

    std::size_t used = 0;
    std::vector<bool> case_used(cases.size(), false);
    for (const auto& path : files) {
        const auto name = path.filename().string();
        std::vector<Parse> parsers;
        std::size_t index = 0;
        for (const auto& c : cases) {
            if (fnmatch(std::string{c.glob}.c_str(), name.c_str(), 0) == 0 &&
                std::find(parsers.begin(), parsers.end(), c.parse) == parsers.end()) {
                parsers.push_back(c.parse);
                case_used[index] = true;
            }
            ++index;
        }
        if (parsers.empty()) {
            ADD_FAILURE() << std::string{venue} << "/" << name << " matches no parser";
            continue;
        }
        SCOPED_TRACE(std::string{venue} + "/" + name);
        t_fixture = std::string{venue} + "/" + name;
        const auto frame = load(path);
        ASSERT_FALSE(frame.empty());
        mutate(Feeder{std::move(parsers)}, frame, k_seed ^ fnv1a(name));
        ++used;
    }
    EXPECT_GT(used, 0U);
    std::size_t index = 0;
    for (const auto& c : cases) {
        EXPECT_TRUE(case_used[index++]) << std::string{venue} << ": " << std::string{c.glob} << " matches no fixture";
    }
}

} // namespace raikiri::wire::malformed
