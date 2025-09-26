#pragma once

// WEEX contract REST request building.
//
// PUBLIC ONLY, AND THERE ARE TWO CALLS. Both are unauthenticated, so there is
// no signer here and no signing rule to get wrong; order entry and the account
// reads are not implemented for this venue and the caps row says so rather than
// this file carrying half of one.
//
// Two endpoints WEEX does offer are deliberately absent, because a builder for
// a request no caller makes is a facility nobody exercises and everybody
// trusts:
//
//   /capi/v3/market/depth answers {asks, bids, lastUpdateId}, and no book needs
//   it — the depth channel opens with its own snapshot and reopens with
//   another.
//
//   /capi/v3/market/time answers {serverTime}. A venue clock matters to a
//   SIGNATURE, which is refused for being stamped outside the venue's window;
//   nothing here signs, so measuring the offset would measure nothing.

#include <raikiri/net/rest_client.hpp>

#include <string_view>

namespace raikiri::venues::weex {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest instruments() noexcept;
    [[nodiscard]] net::RestRequest premium_index() noexcept;
};

} // namespace raikiri::venues::weex
