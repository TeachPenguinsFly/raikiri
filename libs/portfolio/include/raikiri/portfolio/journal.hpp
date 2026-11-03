#pragma once

// the journal's copy of what a fill did to a book

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/portfolio/position.hpp>
#include <raikiri/schema/codec.hpp>

namespace raikiri::portfolio {

void append_pnl(journal::JournalWriter& writer,
                const OrderEvent& event,
                const Instrument& listed,
                raikiri_journal::types::pnlKindEnum kind,
                FixedE9 amount) noexcept;

// what the desk earned on an internal fill, to its own book. the store keys a row on its exec id, so a row beside the
// client's fee row needs one of its own
void append_desk_fee(journal::JournalWriter& writer,
                     std::uint16_t desk_book,
                     const OrderEvent& event,
                     const Instrument& listed,
                     FixedE9 amount) noexcept;

void append_position(journal::JournalWriter& writer, const Position& position, const Instrument& listed) noexcept;

} // namespace raikiri::portfolio
