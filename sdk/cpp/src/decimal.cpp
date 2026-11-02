#include <raikiri/sdk/decimal.hpp>

#include <limits>

namespace raikiri::sdk {

namespace {

__extension__ using Wide = __int128;
__extension__ using UnsignedWide = unsigned __int128;

constexpr std::int64_t k_scale = 1'000'000'000;

std::int64_t checked_narrow(Wide wide) {
    if (wide > std::numeric_limits<std::int64_t>::max() || wide < std::numeric_limits<std::int64_t>::min()) {
        throw std::overflow_error{"Decimal out of range"};
    }
    return static_cast<std::int64_t>(wide);
}

} // namespace

std::expected<Decimal, std::string> Decimal::parse(std::string_view text) {
    const auto refuse = [text](std::string_view why) {
        return std::unexpected(std::string{why} + ": '" + std::string{text} + "'");
    };
    auto rest = text;
    bool negative = false;
    if (!rest.empty() && (rest.front() == '-' || rest.front() == '+')) {
        negative = rest.front() == '-';
        rest.remove_prefix(1);
    }
    if (rest.empty() || rest == ".") {
        return refuse("not a decimal");
    }
    Wide value = 0;
    int places = -1;
    for (const char c : rest) {
        if (c == '.') {
            if (places >= 0) {
                return refuse("not a decimal");
            }
            places = 0;
            continue;
        }
        if (c < '0' || c > '9') {
            return refuse("not a decimal");
        }
        if (places >= 0) {
            ++places;
            if (places > 9) {
                return refuse("more than nine decimal places");
            }
        }
        value = value * 10 + (c - '0');
        if (value > static_cast<Wide>(std::numeric_limits<std::int64_t>::max()) * 10) {
            return refuse("out of range");
        }
    }
    for (int i = places < 0 ? 0 : places; i < 9; ++i) {
        value *= 10;
    }
    if (negative) {
        value = -value;
    }
    if (value > std::numeric_limits<std::int64_t>::max() || value < std::numeric_limits<std::int64_t>::min()) {
        return refuse("out of range");
    }
    return from_raw(static_cast<std::int64_t>(value));
}

std::string Decimal::str() const {
    const auto wide = static_cast<Wide>(raw_);
    const auto magnitude = static_cast<UnsignedWide>(wide < 0 ? -wide : wide);
    const auto whole = static_cast<std::uint64_t>(magnitude / k_scale);
    auto fraction = static_cast<std::uint64_t>(magnitude % k_scale);
    auto out = std::string{raw_ < 0 ? "-" : ""} + std::to_string(whole);
    if (fraction == 0) {
        return out;
    }
    auto digits = std::to_string(fraction);
    digits.insert(0, 9 - digits.size(), '0');
    digits.erase(digits.find_last_not_of('0') + 1);
    return out + "." + digits;
}

Decimal operator*(Decimal a, Decimal b) {
    return Decimal::from_raw(checked_narrow(static_cast<Wide>(a.raw()) * b.raw() / k_scale));
}

Decimal operator/(Decimal a, Decimal b) {
    if (b.raw() == 0) {
        throw std::domain_error{"Decimal division by zero"};
    }
    return Decimal::from_raw(checked_narrow(static_cast<Wide>(a.raw()) * k_scale / b.raw()));
}

} // namespace raikiri::sdk
