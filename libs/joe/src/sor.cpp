#include <raikiri/joe/sor.hpp>

#include <algorithm>
#include <optional>

namespace raikiri::joe {
namespace internal {

// price after the taker fee, e18
[[nodiscard]] int128_t effective(Side side, FixedE9 price, FixedE9 fee) noexcept {
    return static_cast<int128_t>(price.raw) * (side == Side::Buy ? k_e9 + fee.raw : k_e9 - fee.raw);
}

[[nodiscard]] bool better(Side side, int128_t a, int128_t b) noexcept {
    return side == Side::Buy ? a < b : a > b;
}

[[nodiscard]] bool within(Side side, FixedE9 price, FixedE9 limit) noexcept {
    if (limit.is_undef()) {
        return true;
    }
    return side == Side::Buy ? price <= limit : price >= limit;
}

struct Walk {
    FixedE9 worst{};
    FixedE9 value{FixedE9::from_raw(0)};
};

// takes `qty` past the first `from` units of the book; nullopt when the book within the limit is
// too thin
[[nodiscard]] std::optional<Walk>
walk(const VenueBook& book, Side side, FixedE9 limit, std::int64_t from, std::int64_t qty) noexcept {
    Walk out{};
    std::int64_t skip = from;
    std::int64_t need = qty;
    for (const auto& level : book.levels) {
        if (need == 0) {
            break;
        }
        if (!within(side, level.price, limit) || level.qty.raw <= 0) {
            break;
        }
        auto here = level.qty.raw;
        const auto skipped = std::min(skip, here);
        skip -= skipped;
        here -= skipped;
        const auto take = std::min(here, need);
        if (take > 0) {
            out.worst = level.price;
            out.value = out.value + value_of(FixedE9::from_raw(take), level.price);
            need -= take;
        }
    }
    if (need > 0) {
        return std::nullopt;
    }
    return out;
}

struct Piece {
    std::size_t venue{0};
    std::size_t level{0};
    int128_t effective{0};
};

} // namespace internal

Route route(Side side, FixedE9 qty, FixedE9 limit, std::span<const VenueBook> books) {
    Route out{};
    if (qty.is_undef() || qty.raw <= 0) {
        return out;
    }
    std::vector<internal::Piece> pieces;
    for (std::size_t v = 0; v < books.size(); ++v) {
        if (books[v].lot.is_undef() || books[v].lot.raw <= 0) {
            continue;
        }
        for (std::size_t l = 0; l < books[v].levels.size(); ++l) {
            const auto& level = books[v].levels[l];
            if (level.qty.raw <= 0 || level.price.raw <= 0 || !internal::within(side, level.price, limit)) {
                break;
            }
            pieces.push_back(
                {.venue = v, .level = l, .effective = internal::effective(side, level.price, books[v].taker_fee)});
        }
    }
    std::ranges::stable_sort(
        pieces, [side](const auto& a, const auto& b) { return internal::better(side, a.effective, b.effective); });

    std::vector<std::int64_t> alloc(books.size(), 0);
    auto left = qty.raw;
    for (const auto& piece : pieces) {
        if (left == 0) {
            break;
        }
        const auto take = std::min(books[piece.venue].levels[piece.level].qty.raw, left);
        alloc[piece.venue] += take;
        left -= take;
    }

    const auto clears_minimum = [&](std::size_t v, std::int64_t units) {
        const auto walked = internal::walk(books[v], side, limit, 0, units);
        return walked.has_value() && (books[v].min_notional.is_undef() || walked->value >= books[v].min_notional);
    };
    std::int64_t placed = 0;
    for (std::size_t v = 0; v < books.size(); ++v) {
        alloc[v] = alloc[v] == 0 ? 0 : round_down(FixedE9::from_raw(alloc[v]), books[v].lot).raw;
        if (alloc[v] > 0 && !clears_minimum(v, alloc[v])) {
            alloc[v] = 0;
        }
        placed += alloc[v];
    }

    // rounding and minimums leave a remainder; give it in whole lots to the cheapest next unit
    auto residual = qty.raw - placed;
    while (residual > 0) {
        std::optional<std::size_t> pick;
        std::int64_t pick_chunk = 0;
        int128_t pick_effective = 0;
        for (std::size_t v = 0; v < books.size(); ++v) {
            const auto lot = books[v].lot.raw;
            if (lot <= 0 || lot > residual) {
                continue;
            }
            std::int64_t depth = 0;
            std::int64_t room = 0;
            for (const auto& level : books[v].levels) {
                if (!internal::within(side, level.price, limit) || level.qty.raw <= 0) {
                    break;
                }
                depth += level.qty.raw;
                if (depth > alloc[v]) {
                    room = depth - alloc[v];
                    break;
                }
            }
            if (room == 0) {
                continue;
            }
            auto chunk = round_down(FixedE9::from_raw(std::min(room, residual)), books[v].lot).raw;
            chunk = std::max(chunk, lot);
            const auto walked = internal::walk(books[v], side, limit, alloc[v], chunk);
            if (!walked.has_value() || !clears_minimum(v, alloc[v] + chunk)) {
                continue;
            }
            const auto eff = internal::effective(side, walked->worst, books[v].taker_fee);
            if (!pick.has_value() || internal::better(side, eff, pick_effective)) {
                pick = v;
                pick_chunk = chunk;
                pick_effective = eff;
            }
        }
        if (!pick.has_value()) {
            break;
        }
        alloc[*pick] += pick_chunk;
        residual -= pick_chunk;
    }

    for (std::size_t v = 0; v < books.size(); ++v) {
        if (alloc[v] == 0) {
            continue;
        }
        const auto walked = internal::walk(books[v], side, limit, 0, alloc[v]);
        if (!walked.has_value()) {
            continue;
        }
        const auto leg_qty = FixedE9::from_raw(alloc[v]);
        out.legs.push_back({.instrument = books[v].instrument,
                            .lot = books[v].lot,
                            .lots = lots_in(leg_qty, books[v].lot),
                            .limit = walked->worst,
                            .qty = leg_qty,
                            .value = walked->value,
                            .fee = value_of(walked->value, books[v].taker_fee)});
    }
    out.unrouted = FixedE9::from_raw(residual);
    return out;
}

} // namespace raikiri::joe
