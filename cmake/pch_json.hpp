#pragma once

// For targets that link simdjson: the standard headers plus the parser. Only
// those targets — see the INVARIANT in cmake/Pch.cmake.

#include <simdjson.h>

#include "pch_std.hpp"
