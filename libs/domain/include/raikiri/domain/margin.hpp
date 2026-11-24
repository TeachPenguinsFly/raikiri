#pragma once

// leverage and margin mode of one instrument on a venue account; the request and its answer cross shm

#include <raikiri/common/time.hpp>
#include <raikiri/domain/error_class.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace raikiri {

enum class MarginMode : std::uint8_t { Unknown = 0, Cross, Isolated };

[[nodiscard]] constexpr std::string_view to_string(MarginMode mode) noexcept {
    switch (mode) {
    case MarginMode::Cross:
        return "cross";
    case MarginMode::Isolated:
        return "isolated";
    case MarginMode::Unknown:
        break;
    }
    return {};
}

// zero and Unknown mean the venue did not say
struct MarginState {
    std::uint32_t leverage{0};
    std::uint32_t max_leverage{0};
    MarginMode mode{MarginMode::Unknown};
    std::uint8_t pad_[3]{};

    friend constexpr bool operator==(const MarginState& a, const MarginState& b) noexcept {
        return a.leverage == b.leverage && a.max_leverage == b.max_leverage && a.mode == b.mode;
    }
};

// where a margin-mode change lands: the one instrument, or every symbol of the account with it
enum class MarginScope : std::uint8_t { Unknown = 0, OneInstrument, WholeAccount };

[[nodiscard]] constexpr std::string_view to_string(MarginScope scope) noexcept {
    switch (scope) {
    case MarginScope::OneInstrument:
        return "instrument";
    case MarginScope::WholeAccount:
        return "account";
    case MarginScope::Unknown:
        break;
    }
    return {};
}

enum class MarginOp : std::uint8_t { Read = 0, Set };

// a request older than this is answered Failed and never reaches the venue: its asker has stopped waiting
inline constexpr std::uint64_t k_margin_request_ttl_ns = 5'000'000'000ULL;

struct MarginRequest {
    std::uint64_t id{0};
    InstrumentId instrument{};
    UnixNanos ts_created{};
    std::uint32_t leverage{0}; // zero leaves it alone
    MarginOp op{MarginOp::Read};
    MarginMode mode{MarginMode::Unknown}; // Unknown leaves it alone
    std::uint8_t pad_[2]{};
};

static_assert(sizeof(MarginRequest) == 32);
static_assert(std::is_trivially_copyable_v<MarginRequest>);

enum class MarginOutcome : std::uint8_t {
    Done = 0,
    Refused,     // the venue, or its known maximum, said no; `message` is the reason
    Unsupported, // this venue has no such call here
    Failed,      // the call did not complete; `cls` says how
};

struct MarginReply {
    std::uint64_t id{0};
    InstrumentId instrument{};
    MarginState before{};
    // equal to `before` where nothing changed; after a refusal it is what the venue holds now
    MarginState after{};
    MarginOutcome outcome{MarginOutcome::Done};
    ErrorClass cls{};
    MarginScope scope{MarginScope::Unknown}; // of the venue's margin mode, whatever the request
    std::uint8_t pad_[5]{};
    std::int64_t venue_code{0};
    char message[104]{}; // nul-terminated

    [[nodiscard]] std::string_view reason() const noexcept { return std::string_view{message}; }
};

static_assert(sizeof(MarginReply) == 160);
static_assert(std::is_trivially_copyable_v<MarginReply>);

} // namespace raikiri
