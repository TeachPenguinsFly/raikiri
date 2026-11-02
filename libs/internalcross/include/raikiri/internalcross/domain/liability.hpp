#pragma once

// what the desk carries for its clients on one listing; a cross moves lots between books and none at the venue, so the
// sum of books still has to equal the venue, and the part of the clients' positions facing each other has no venue
// position behind it at all

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/origin.hpp>
#include <raikiri/portfolio/position.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace raikiri::internalcross {

struct LiabilityRow {
    InstrumentId instrument{};
    std::int64_t client_long{0};
    std::int64_t client_short{0};
    std::int64_t hedge{0};
    std::int64_t others{0};

    // what reconciliation compares with the venue
    [[nodiscard]] constexpr std::int64_t book_sum() const noexcept {
        return client_long - client_short + hedge + others;
    }

    // client lots whose only backing is another client's opposite position on this desk
    [[nodiscard]] constexpr std::int64_t offset() const noexcept {
        return client_long < client_short ? client_long : client_short;
    }
};

[[nodiscard]] std::vector<LiabilityRow> liability_rows(std::span<const portfolio::Position> positions,
                                                       const std::function<BookKind(std::uint16_t)>& kind_of);

} // namespace raikiri::internalcross
