#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace raikiri::clientapi {

// each field on its own line: run together, "/a" + "b=1" would sign the same bytes as "/ab=1" + ""
[[nodiscard]] std::string canonical_request(std::string_view timestamp_ms,
                                            std::string_view method,
                                            std::string_view path,
                                            std::string_view query,
                                            std::string_view body);

[[nodiscard]] std::optional<std::uint64_t> parse_millis(std::string_view text) noexcept;

struct BucketView {
    bool allowed{false};
    std::uint32_t limit{0};
    std::uint32_t remaining{0};
    std::uint32_t reset_s{0};
    std::uint32_t retry_after_s{0};
};

// holds nano-tokens so a low rate still refills between two calls
class TokenBucket {
public:
    TokenBucket(std::uint32_t per_second, std::uint32_t burst, std::uint64_t now_ns) noexcept;

    BucketView take(std::uint64_t now_ns) noexcept;

private:
    std::uint64_t per_second_;
    std::uint64_t capacity_;
    std::uint64_t tokens_;
    std::uint64_t last_ns_;
    std::uint32_t burst_;
};

// a signature is single use while its timestamp could still pass the skew check
class ReplayGuard {
public:
    explicit ReplayGuard(std::uint64_t window_ms) noexcept : window_ms_{window_ms} {}

    [[nodiscard]] bool admit(std::string_view signature, std::uint64_t now_ms);

    [[nodiscard]] std::size_t size() const noexcept { return seen_.size(); }

private:
    struct Seen {
        std::string signature;
        std::uint64_t expires_ms{0};
    };

    std::uint64_t window_ms_;
    std::deque<Seen> order_;
    std::unordered_set<std::string> seen_;
};

struct StoredReply {
    int status{0};
    std::string body{};
};

enum class IdemState : std::uint8_t {
    Pending, // the call is running, or a crash interrupted it
    Done,
    Unknown, // the backend took the call and never answered
};

struct IdemEntry {
    std::string request_hash{};
    IdemState state{IdemState::Pending};
    StoredReply reply{};
    std::uint64_t at_ns{0};
};

enum class IdemVerdict : std::uint8_t { Fresh, InFlight, Replay, Mismatch, OutcomeUnknown };

struct IdemBegin {
    IdemVerdict verdict{IdemVerdict::Fresh};
    StoredReply reply{};
};

// the same key with the same request answers the first result; with another request it is refused
class IdempotencyTable {
public:
    explicit IdempotencyTable(std::uint64_t ttl_ns) noexcept : ttl_ns_{ttl_ns} {}

    [[nodiscard]] IdemBegin begin(const std::string& key, std::string_view request_hash, std::uint64_t now_ns);
    void finish(const std::string& key, StoredReply reply);
    void mark_unknown(const std::string& key);
    void release(const std::string& key);

    // a pending entry comes back Unknown: whatever it was doing, the process died during it
    void restore(const std::string& key, IdemEntry entry, std::uint64_t now_ns);
    [[nodiscard]] const IdemEntry* find(const std::string& key) const;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    void expire(std::uint64_t now_ns);

    std::uint64_t ttl_ns_;
    std::unordered_map<std::string, IdemEntry> entries_;
    std::deque<std::pair<std::string, std::uint64_t>> order_;
};

} // namespace raikiri::clientapi
