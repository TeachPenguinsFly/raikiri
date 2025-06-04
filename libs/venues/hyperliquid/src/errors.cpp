#include <raikiri/venues/hyperliquid/errors.hpp>

namespace raikiri::venues::hyperliquid {

namespace {

[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

bool is_null_body(std::string_view body) noexcept {
    return trim(body) == "null";
}

exec::ErrorClass classify(int status, std::string_view body) noexcept {
    // Status 0 is a transport failure: connectivity, not a venue verdict.
    if (status == 0) {
        return exec::ErrorClass::Connectivity;
    }
    if (status >= 200 && status < 300) {
        return is_null_body(body) ? exec::ErrorClass::Terminal : exec::ErrorClass::Retryable;
    }
    if (status == 429) {
        return exec::ErrorClass::RateLimited;
    }
    if (status >= 500) {
        return exec::ErrorClass::Retryable;
    }
    if (status == 401 || status == 403) {
        return exec::ErrorClass::AuthFailure;
    }
    // 400/405/415/422 and anything unrecognised: our request is wrong.
    return exec::ErrorClass::Terminal;
}


exec::ErrorClass classify_reason(std::string_view reason) noexcept {
    const auto text = trim(reason);
    if (text.empty()) {
        return exec::ErrorClass::Terminal;
    }
    // The venue's own rate limiter, which is priced in traded volume rather
    // than in time: "Too many cumulative requests sent (37986 > 10436) for
    // cumulative volume traded $437.92."
    if (text.starts_with("Too many cumulative requests")) {
        return exec::ErrorClass::RateLimited;
    }
    // Two spellings of the same disaster, and both arrive with HTTP 200.
    //
    // "Unable to recover signer." is r/s that are not a signature at all —
    // VERIFIED live against /exchange. A signature that IS well formed but
    // recovers to an address the venue has never seen answers "User or API
    // Wallet 0x... does not exist" instead, naming a different stranger every
    // attempt. Both are what a wrong keccak, a wrong msgpack field order or a
    // wrong EIP-712 domain look like from outside, and neither improves on a
    // retry: nothing this key signs will ever be taken.
    if (text.starts_with("Unable to recover signer") || text.starts_with("User or API Wallet") ||
        text.starts_with("L1 error: User or API Wallet")) {
        return exec::ErrorClass::AuthFailure;
    }
    // Every order will fail identically until a human funds the account, so
    // this halts rather than retrying the whole book one order at a time.
    if (text.starts_with("Must deposit before performing actions")) {
        return exec::ErrorClass::AuthFailure;
    }
    // Everything else the venue names is a property of the ORDER — tick size,
    // minimum notional, margin, reduce-only, open-interest caps, oracle
    // distance, an ALO that would have crossed, an IOC that found nothing.
    // Resending the same order reproduces the same refusal.
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::string_view reason) noexcept {
    const auto text = trim(reason);
    return text.starts_with("Order was never placed, already canceled, or filled") ||
           text.starts_with("TWAP was never placed, already canceled, or filled");
}

} // namespace raikiri::venues::hyperliquid
