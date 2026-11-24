#include <raikiri/exec/margin.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace raikiri::exec {

namespace {

// no venue numbers a refusal like this
constexpr std::int64_t k_unsupported_code = std::numeric_limits<std::int64_t>::min();

void say(MarginReply& reply, std::string_view text) noexcept {
    const auto n = std::min(text.size(), sizeof reply.message - 1);
    text.copy(reply.message, n);
    reply.message[n] = '\0';
}

void fail(MarginReply& reply, const VenueError& error) noexcept {
    reply.cls = error.cls;
    if (error.venue_code == k_unsupported_code) {
        reply.outcome = MarginOutcome::Unsupported;
    } else {
        reply.outcome = error.cls == ErrorClass::Terminal ? MarginOutcome::Refused : MarginOutcome::Failed;
        reply.venue_code = error.venue_code;
    }
    say(reply, error.message.view());
}

} // namespace

VenueError margin_unsupported(std::string_view what) noexcept {
    return VenueError::make(ErrorClass::Terminal, k_unsupported_code, what);
}

MarginReply serve_margin(MarginControl* control, const MarginRequest& request, UnixNanos now) noexcept {
    MarginReply reply{};
    reply.id = request.id;
    reply.instrument = request.instrument;
    if (control == nullptr) {
        reply.outcome = MarginOutcome::Unsupported;
        say(reply, "leverage and margin mode are not implemented for this venue");
        return reply;
    }
    reply.scope = control->mode_scope();
    if (now.ns > request.ts_created.ns + k_margin_request_ttl_ns) {
        reply.outcome = MarginOutcome::Failed;
        reply.cls = ErrorClass::Retryable;
        say(reply, "the request expired before the gateway read it; nothing was sent to the venue");
        return reply;
    }
    const auto before = control->margin_state(request.instrument);
    if (!before.has_value()) {
        fail(reply, before.error());
        return reply;
    }
    reply.before = *before;
    reply.after = *before;
    if (request.op == MarginOp::Read) {
        return reply;
    }

    if (request.leverage != 0 && before->max_leverage != 0 && request.leverage > before->max_leverage) {
        reply.outcome = MarginOutcome::Refused;
        char text[96];
        std::snprintf(text,
                      sizeof text,
                      "leverage %u is above this instrument's maximum of %u",
                      request.leverage,
                      before->max_leverage);
        say(reply, text);
        return reply;
    }

    // what the venue holds as far as the calls so far say; the second call is given it
    auto held = *before;
    bool sent = false;
    std::expected<void, VenueError> done{};
    if (request.mode != MarginMode::Unknown && request.mode != held.mode) {
        sent = true;
        done = control->set_margin_mode(request.instrument, request.mode, held);
        if (done.has_value()) {
            held.mode = request.mode;
        }
    }
    if (done.has_value() && request.leverage != 0 && request.leverage != held.leverage) {
        sent = true;
        done = control->set_leverage(request.instrument, request.leverage, held);
        if (done.has_value()) {
            held.leverage = request.leverage;
        }
    }
    if (!done.has_value()) {
        fail(reply, done.error());
    }
    if (sent) {
        // the venue's word over ours; where it will not answer, what the accepted calls add up to
        const auto after = control->margin_state(request.instrument);
        reply.after = after.has_value() ? *after : held;
    }
    return reply;
}

} // namespace raikiri::exec
