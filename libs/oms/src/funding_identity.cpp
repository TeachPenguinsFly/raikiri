#include <raikiri/oms/funding_identity.hpp>

#include <charconv>

namespace raikiri::oms {

namespace {

// returns nullptr once anything would not fit
[[nodiscard]] char* put(char* out, char* end, std::uint64_t value, char separator) noexcept {
    if (out == nullptr) {
        return nullptr;
    }
    const auto [stop, ec] = std::to_chars(out, end, value);
    if (ec != std::errc{} || stop == end) {
        return nullptr;
    }
    *stop = separator;
    return stop + 1;
}

} // namespace

std::string_view funding_identity(InstrumentId instrument,
                                  UnixNanos settled_at,
                                  std::uint16_t strategy_id,
                                  std::span<char> into) noexcept {
    if (into.size() < k_max_funding_identity) {
        return {};
    }
    char* const begin = into.data();
    char* const end = begin + into.size();
    char* out = put(begin, end, instrument.packed, ':');
    out = put(out, end, settled_at.ns / 1'000'000'000ULL, ':');
    if (out == nullptr) {
        return {};
    }
    const auto [stop, ec] = std::to_chars(out, end, strategy_id);
    if (ec != std::errc{}) {
        return {};
    }
    return std::string_view{begin, static_cast<std::size_t>(stop - begin)};
}

} // namespace raikiri::oms
