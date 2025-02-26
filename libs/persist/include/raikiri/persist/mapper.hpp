#pragma once

// Pure journal-frame → row mapping. No SQL, no I/O — fully unit-testable.

#include <raikiri/journal/journal.hpp>
#include <raikiri/persist/rows.hpp>

namespace raikiri::persist {

// Appends rows derived from one frame into the batch (a frame may produce
// zero rows — MD frames — or several: a fill yields an event row, an order
// upsert, and an execution row). Updates batch.last_seq.
void map_frame(const journal::FrameView& frame, RowBatch& batch);

} // namespace raikiri::persist
