#pragma once

// the durable document: what people decided, and each book's state

#include <raikiri/bookrisk/limits.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/oms/book_state.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace raikiri::bookrisk {

struct StateRecord {
    oms::BookState state{oms::BookState::Active};
    std::string reason{};
    std::string by{};
    UnixNanos since{};
};

// the book's p&l when this utc day began, so a restart mid-day keeps the day's loss
struct DayMark {
    std::uint32_t day{0};
    FixedE9 start_pnl{FixedE9::from_raw(0)};
};

struct BookSettings {
    LimitRow limits{};
    StateRecord state{};
    DayMark day{};
};

struct DeskSettings {
    // undef until a person declares it, and no new order passes until then
    FixedE9 capital{FixedE9::undef()};
    std::string capital_by{};
    UnixNanos capital_at{};
    LimitRow defaults{};
    // only active or halted: the switch over everything
    StateRecord desk{};
    std::map<BookId, BookSettings> books{};
};

} // namespace raikiri::bookrisk
