#include <raikiri/internalcross/infrastructure/journal_log.hpp>
#include <raikiri/oms/replay.hpp>
#include <raikiri/portfolio/journal.hpp>

namespace raikiri::internalcross {

void JournalCrossLog::order(const OrderCmd& cmd, const OrderOrigin& origin) noexcept {
    oms::append_order_cmd(*writer_, cmd, origin, BookKind::Hedge);
}

void JournalCrossLog::crossed(const oms::CrossRecord& record,
                              const BookedLeg& buy,
                              const BookedLeg& sell,
                              const Instrument& listed) noexcept {
    namespace jt = raikiri_journal::types;
    oms::append_cross(*writer_, record);
    for (const auto* leg : {&buy, &sell}) {
        oms::append_order_event(*writer_, leg->event);
        portfolio::append_pnl(*writer_, leg->event, listed, jt::pnlKindEnum::Realized, leg->applied.realized);
        const auto fee = FixedE9::from_raw(-(leg->applied.close_fee.raw + leg->applied.open_fee.raw));
        portfolio::append_pnl(*writer_, leg->event, listed, jt::pnlKindEnum::Fee, fee);
        if (desk_book_.has_value()) {
            // what the client paid, so that every book together adds up to the venue account
            portfolio::append_desk_fee(*writer_, *desk_book_, leg->event, listed, FixedE9::from_raw(-fee.raw));
        }
        if (leg->position != nullptr) {
            portfolio::append_position(*writer_, *leg->position, listed);
        }
    }
}

} // namespace raikiri::internalcross
