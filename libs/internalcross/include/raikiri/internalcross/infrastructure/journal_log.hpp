#pragma once

#include <raikiri/internalcross/application/ports.hpp>
#include <raikiri/journal/writer.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::internalcross {

// the cross frame first: both legs and both positions are whole in it, whatever a crash leaves of what follows
class JournalCrossLog final : public CrossLog {
public:
    // no desk book leaves the internal fees in the cross frames alone
    explicit JournalCrossLog(journal::JournalWriter& writer, std::optional<std::uint16_t> desk_book = {}) noexcept
        : writer_{&writer}, desk_book_{desk_book} {}

    void order(const OrderCmd& cmd, const OrderOrigin& origin) noexcept override;
    void crossed(const oms::CrossRecord& record,
                 const BookedLeg& buy,
                 const BookedLeg& sell,
                 const Instrument& listed) noexcept override;

private:
    journal::JournalWriter* writer_;
    std::optional<std::uint16_t> desk_book_;
};

} // namespace raikiri::internalcross
