#include <raikiri/options/key.hpp>

#include <array>
#include <cstdint>
#include <format>

namespace raikiri::options {

namespace {

constexpr std::int64_t k_ns_per_day = 86'400LL * 1'000'000'000LL;

struct Civil {
    std::int64_t year{0};
    unsigned month{0};
    unsigned day{0};
};

// howard hinnant's civil_from_days
[[nodiscard]] constexpr Civil civil_from_days(std::int64_t days) noexcept {
    days += 719'468;
    const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
    const auto doe = static_cast<unsigned>(days - (era * 146'097));
    const unsigned yoe = (doe - (doe / 1'460) + (doe / 36'524) - (doe / 146'096)) / 365;
    const unsigned doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
    const unsigned mp = ((5 * doy) + 2) / 153;
    const unsigned day = doy - (((153 * mp) + 2) / 5) + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    return {static_cast<std::int64_t>(yoe) + (era * 400) + (month <= 2 ? 1 : 0), month, day};
}

} // namespace

std::optional<OptionKey> key_of(const OptionInstrument& option) noexcept {
    const auto& base = option.base;
    if (base.underlying.empty() || option.expiry.ns == 0 || option.strike.is_undef() || option.strike.raw <= 0 ||
        base.tick_size.is_undef() || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    return OptionKey{.underlying = base.underlying,
                     .expiry = option.expiry,
                     .strike = base.ticks_to_price(option.strike),
                     .put_call = option.put_call};
}

std::string canonical_name(const OptionKey& key) {
    const auto date = civil_from_days(static_cast<std::int64_t>(key.expiry.ns) / k_ns_per_day);
    std::array<char, 32> buffer{};
    const auto strike = format_e9(key.strike, buffer.data(), buffer.size());
    return std::format("{}-{:04}{:02}{:02}-{}-{}",
                       key.underlying.view(),
                       date.year,
                       date.month,
                       date.day,
                       strike,
                       key.put_call == PutCall::Call ? 'C' : 'P');
}

} // namespace raikiri::options
