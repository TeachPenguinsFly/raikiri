#pragma once

// The Mexc adapter.
//
// Public futures market data off contract.mexc.com, and order entry for both
// markets over api.mexc.com — two signing schemes on one host, plus a futures
// private socket that authenticates itself rather than minting a listen key.
//
// MEXC gates futures order placement behind an account permission ("Order
// Placing", KYC-bound). A key without it is refused with a permission code
// this adapter classifies as AuthFailure, so the venue halts loudly instead of
// leaving orders in doubt.
//
// SPOT IS WRITE-ONLY HERE. create_order and cancel_order serve spot, but
// open_order_reports(Spot) and fill_reports(Spot) refuse: MEXC's spot tree
// has no endpoint this adapter reads. So a spot order can be placed and no
// pull will ever find it, and reconciliation on spot is blind by construction
// rather than by failure. The refusals are deliberate — answering an empty
// list would tell the reconciler the account is flat, which is the one lie
// that costs money. Until a spot pull exists, run MEXC spot only where the
// journal is the record of truth.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::mexc {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::mexc
