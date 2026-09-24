#pragma once

// journal frame to row mapping, with no sql or i/o

#include <raikiri/journal/journal.hpp>
#include <raikiri/persist/rows.hpp>

namespace raikiri::persist {

// a frame may yield zero rows or several; updates batch.last_seq
void map_frame(const journal::FrameView& frame, RowBatch& batch);

} // namespace raikiri::persist
