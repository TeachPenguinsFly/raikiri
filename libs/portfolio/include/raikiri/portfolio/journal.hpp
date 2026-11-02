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

void append_position(journal::JournalWriter& writer, const Position& position, const Instrument& listed) noexcept;

} // namespace raikiri::portfolio
