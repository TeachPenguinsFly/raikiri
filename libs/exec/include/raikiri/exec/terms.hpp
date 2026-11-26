#pragma once

// fees and maintenance tiers from the venue; cold calls, made by whichever adapter serves the account's pulls

#include <raikiri/domain/terms.hpp>
#include <raikiri/exec/error.hpp>

#include <expected>
#include <optional>
#include <vector>

namespace raikiri::exec {

struct BaseFees {
    double maker{0};
    double taker{0};
};

// the venue's public base tier, from fee_schedule.cpp; nullopt where no reviewed row exists
[[nodiscard]] std::optional<BaseFees> base_fee_schedule(VenueId venue, MarketType market) noexcept;

// a refusal no retry will change, a key without the permission say: the answer goes on without that half
[[nodiscard]] constexpr bool terms_go_without(const VenueError& error) noexcept {
    return error.cls == ErrorClass::Terminal || error.cls == ErrorClass::AuthFailure;
}

class TermsSource {
public:
    TermsSource() = default;
    TermsSource(const TermsSource&) = delete;
    TermsSource& operator=(const TermsSource&) = delete;
    virtual ~TermsSource() = default;

    // fee_source stays Unknown where the venue was not asked or has no per-account answer
    [[nodiscard]] virtual std::expected<TradingTerms, VenueError> trading_terms(InstrumentId instrument) = 0;
};

// asks the venue, falls back to the schedule for fees, and cuts the answer into ring records
// a null source is a venue without the calls: schedule fees and no tiers
[[nodiscard]] std::vector<TermsReply> serve_terms(TermsSource* source, const TermsRequest& request, UnixNanos now);

} // namespace raikiri::exec
