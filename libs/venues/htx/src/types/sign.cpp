#include <raikiri/venues/htx/sign.hpp>

#include <algorithm>
#include <cstring>

namespace raikiri::venues::htx::sign {

namespace {

constexpr std::string_view k_hex = "0123456789ABCDEF";

[[nodiscard]] bool unreserved(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

// Days since 1970-01-01 -> proleptic Gregorian y/m/d (Hinnant's civil_from_days).
// gmtime_r would answer the same question through a locale-aware path that is
// neither noexcept nor free of global state.
struct Civil {
    std::int64_t year{0};
    unsigned month{0};
    unsigned day{0};
};

[[nodiscard]] Civil civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<std::uint64_t>(z - era * 146097);
    const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint64_t mp = (5 * doy + 2) / 153;
    const std::uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::uint64_t m = mp < 10 ? mp + 3 : mp - 9;
    return {.year = y + static_cast<std::int64_t>(m <= 2),
            .month = static_cast<unsigned>(m),
            .day = static_cast<unsigned>(d)};
}

void put_two(char* out, unsigned value) noexcept {
    out[0] = static_cast<char>('0' + (value / 10) % 10);
    out[1] = static_cast<char>('0' + value % 10);
}

// Appends into a fixed buffer, marking overflow rather than truncating: a
// half-written canonical string authenticates as a bad key.
class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    void put(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return;
        }
        std::memcpy(buf_.data() + len_, text.data(), text.size());
        len_ += text.size();
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return overflow_ ? std::string_view{} : std::string_view{buf_.data(), len_};
    }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

} // namespace

std::string_view encode(std::string_view text, std::span<char> out) noexcept {
    std::size_t len = 0;
    for (const char c : text) {
        if (unreserved(c)) {
            if (len + 1 > out.size()) {
                return {};
            }
            out[len++] = c;
            continue;
        }
        if (len + 3 > out.size()) {
            return {};
        }
        const auto byte = static_cast<std::uint8_t>(c);
        out[len++] = '%';
        out[len++] = k_hex[byte >> 4U];
        out[len++] = k_hex[byte & 0x0FU];
    }
    return {out.data(), len};
}

std::string_view timestamp(std::uint64_t epoch_s, std::span<char> out) noexcept {
    constexpr std::size_t k_len = 19; // YYYY-MM-DDTHH:MM:SS
    if (out.size() < k_len) {
        return {};
    }
    const auto secs = static_cast<std::int64_t>(epoch_s);
    const std::int64_t days = secs / 86400;
    auto rem = secs % 86400;
    const auto civil = civil_from_days(days);
    if (civil.year < 1000 || civil.year > 9999) {
        return {}; // outside the four-digit form HTX parses
    }

    char* p = out.data();
    put_two(p, static_cast<unsigned>(civil.year / 100));
    put_two(p + 2, static_cast<unsigned>(civil.year % 100));
    p[4] = '-';
    put_two(p + 5, civil.month);
    p[7] = '-';
    put_two(p + 8, civil.day);
    p[10] = 'T';
    put_two(p + 11, static_cast<unsigned>(rem / 3600));
    p[13] = ':';
    rem %= 3600;
    put_two(p + 14, static_cast<unsigned>(rem / 60));
    p[16] = ':';
    put_two(p + 17, static_cast<unsigned>(rem % 60));
    return {out.data(), k_len};
}

void Query::add(std::string_view name, std::string_view value) noexcept {
    if (count_ == params_.size()) {
        overflow_ = true;
        return;
    }
    params_[count_++] = {.name = name, .value = value};
}

std::string_view Query::render(std::span<char> out) noexcept {
    if (overflow_) {
        return {};
    }
    // Parameter names are distinct constants, so ordering by name is total.
    std::sort(params_.begin(),
              params_.begin() + static_cast<std::ptrdiff_t>(count_),
              [](const Param& a, const Param& b) { return a.name < b.name; });

    Appender writer{out};
    for (std::size_t i = 0; i < count_; ++i) {
        if (i != 0) {
            writer.put("&");
        }
        writer.put(params_[i].name);
        writer.put("=");
        std::array<char, 192> encoded{};
        const auto value = encode(params_[i].value, encoded);
        if (value.empty() && !params_[i].value.empty()) {
            return {};
        }
        writer.put(value);
    }
    return writer.view();
}

std::string_view canonical(std::string_view method,
                           std::string_view host,
                           std::string_view path,
                           std::string_view query,
                           std::span<char> out) noexcept {
    Appender writer{out};
    writer.put(method);
    writer.put("\n");
    writer.put(host);
    writer.put("\n");
    writer.put(path);
    writer.put("\n");
    writer.put(query);
    return writer.view();
}

} // namespace raikiri::venues::htx::sign
