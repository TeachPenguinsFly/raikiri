#include <raikiri/admin/format.hpp>
#include <raikiri/common/int128.hpp>

#include <algorithm>
#include <array>
#include <charconv>

namespace raikiri::admin {

namespace {

// Taken in 128 bits: a meme coin's book size in e9 passes int64, and the
// checked multiply in lots_to_qty aborts there.
std::string scaled(std::int64_t count, FixedE9 step) {
    if (step.is_undef()) {
        return {};
    }
    const int128_t product = static_cast<int128_t>(count) * step.raw;
    auto mag = product < 0 ? -static_cast<uint128_t>(product)
                           : static_cast<uint128_t>(product);
    std::string out;
    for (int place = 0; mag != 0 || place <= 9; ++place) {
        if (place == 9) {
            out.push_back('.');
        }
        out.push_back(static_cast<char>('0' + static_cast<int>(mag % 10)));
        mag /= 10;
    }
    if (product < 0) {
        out.push_back('-');
    }
    std::ranges::reverse(out);
    while (out.back() == '0') {
        out.pop_back();
    }
    if (out.back() == '.') {
        out.pop_back();
    }
    return out;
}

} // namespace

std::string digits(std::uint64_t value) {
    std::array<char, 24> buf{};
    const auto [stop, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return ec == std::errc{} ? std::string{buf.data(), static_cast<std::size_t>(stop - buf.data())}
                             : std::string{};
}

std::string decimal(FixedE9 value) {
    std::array<char, 32> buf{};
    const auto text = format_e9(value, buf.data(), buf.size());
    return std::string{text}; // empty when undefined; see the header
}

std::string decimal(FixedE6 value) {
    return value.is_undef() ? std::string{} : scaled(value.raw, FixedE9::from_raw(k_e9 / k_e6));
}

std::string price(PriceTicks ticks, const InstrumentBase& base) {
    return ticks.is_undef() ? std::string{} : scaled(ticks.raw, base.tick_size);
}

std::string size(QtyLots lots, const InstrumentBase& base) {
    return lots.is_undef() ? std::string{} : scaled(lots.raw, base.lot_size);
}

std::string_view contract_kind_name(ContractKind kind) noexcept {
    switch (kind) {
    case ContractKind::Linear:
        return "linear";
    case ContractKind::Inverse:
        return "inverse";
    case ContractKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace raikiri::admin
