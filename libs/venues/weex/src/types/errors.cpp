#include <raikiri/venues/weex/errors.hpp>

namespace raikiri::venues::weex {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_param_empty:
    case k_param_invalid:
        // A request WEEX could not read is one this process built wrongly, and
        // building it again would build it wrongly again.
        return exec::ErrorClass::Terminal;
    default:
        break;
    }
    // NO Retryable ARM, because no public call has produced a code that earns
    // one. WEEX states its allowance as 500 weight per ten minutes per IP and
    // this adapter spends four calls a minute against it, so the throttling
    // refusal has never been provoked and there is no measured code to file it
    // under. An unknown refusal repeated at full speed is how an IP earns a
    // ban; Terminal is the safe default and a caller's next tick still asks.
    return exec::ErrorClass::Terminal;
}

} // namespace raikiri::venues::weex
