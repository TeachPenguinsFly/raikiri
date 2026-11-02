#pragma once

#include <raikiri/internalcross/application/ports.hpp>
#include <raikiri/journal/writer.hpp>

namespace raikiri::internalcross {

// the cross frame first: both legs and both positions are whole in it, whatever a crash leaves of what follows
class JournalCrossLog final : public CrossLog {
public:
    explicit JournalCrossLog(journal::JournalWriter& writer) noexcept : writer_{&writer} {}

    void order(const OrderCmd& cmd, const OrderOrigin& origin) noexcept override;
    void crossed(const oms::CrossRecord& record,
                 const BookedLeg& buy,
                 const BookedLeg& sell,
                 const Instrument& listed) noexcept override;

private:
    journal::JournalWriter* writer_;
};

} // namespace raikiri::internalcross
