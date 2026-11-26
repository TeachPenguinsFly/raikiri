#include <raikiri/exec/terms.hpp>

#include <algorithm>

namespace raikiri::exec {

namespace {

void say(TermsReply& reply, std::string_view text) noexcept {
    const auto n = std::min(text.size(), sizeof reply.message - 1);
    text.copy(reply.message, n);
    reply.message[n] = '\0';
}

} // namespace

std::vector<TermsReply> serve_terms(TermsSource* source, const TermsRequest& request, UnixNanos now) {
    TermsReply head{};
    head.id = request.id;
    head.instrument = request.instrument;
    if (now.ns > request.ts_created.ns + k_terms_request_ttl_ns) {
        head.outcome = TermsOutcome::Failed;
        head.cls = ErrorClass::Retryable;
        say(head, "the request expired before the gateway read it");
        return {head};
    }
    TradingTerms terms{};
    if (source != nullptr) {
        auto asked = source->trading_terms(request.instrument);
        if (!asked.has_value()) {
            head.outcome = TermsOutcome::Failed;
            head.cls = asked.error().cls;
            head.venue_code = asked.error().venue_code;
            say(head, asked.error().message.view());
            return {head};
        }
        terms = std::move(*asked);
    }
    if (terms.fee_source == FeeSource::Unknown) {
        const auto base = base_fee_schedule(request.instrument.venue(), request.instrument.market());
        if (!base.has_value()) {
            head.outcome = TermsOutcome::Unsupported;
            say(head, "no fee source for this venue and market");
            return {head};
        }
        terms.maker_fee = base->maker;
        terms.taker_fee = base->taker;
        terms.fee_source = FeeSource::Schedule;
    }
    head.maker_fee = terms.maker_fee;
    head.taker_fee = terms.taker_fee;
    head.fee_source = terms.fee_source;
    head.tiers_source = terms.tiers.empty() ? TiersSource::Default : terms.tiers_source;
    head.basis = terms.basis;

    const auto total = std::min(terms.tiers.size(), k_terms_max_tiers);
    const auto parts = std::max<std::size_t>(1, (total + k_terms_tiers_per_part - 1) / k_terms_tiers_per_part);
    head.parts = static_cast<std::uint8_t>(parts);
    std::vector<TermsReply> out;
    out.reserve(parts);
    for (std::size_t part = 0; part < parts; ++part) {
        auto reply = head;
        reply.part = static_cast<std::uint8_t>(part);
        const auto first = part * k_terms_tiers_per_part;
        const auto count = std::min(k_terms_tiers_per_part, total - first);
        reply.count = static_cast<std::uint8_t>(count);
        std::copy_n(terms.tiers.begin() + static_cast<std::ptrdiff_t>(first), count, reply.tiers);
        out.push_back(reply);
    }
    return out;
}

} // namespace raikiri::exec
