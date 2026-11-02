#pragma once

#include <raikiri/clientapi/application/ports.hpp>
#include <raikiri/clientapi/application/refusal.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace raikiri::clientapi {

struct SignedRequest {
    std::string_view key_id{};
    std::string_view timestamp{};
    std::string_view signature{};
    std::string_view method{};
    std::string_view path{};
    std::string_view query{};
    std::string_view body{};
    std::string_view peer_ip{};
};

struct AuthPolicy {
    std::uint64_t skew_ms{5'000};
    std::uint32_t bad_auth_per_second{1};
    std::uint32_t bad_auth_burst{30};
};

// checks in this order, so a refusal says nothing about a key the caller cannot sign for:
// timestamp, key and signature (one answer for both), replay, address
class Authenticator {
public:
    Authenticator(const KeyDirectory& keys, WallClock wall, AuthPolicy policy = {})
        : keys_{&keys}, wall_{std::move(wall)}, policy_{policy} {}

    [[nodiscard]] Outcome<std::shared_ptr<const ApiKey>> check(const SignedRequest& request);

    // the key as it stands now, so an open stream follows a reload; null once it is revoked
    [[nodiscard]] std::shared_ptr<const ApiKey> current(std::string_view key_id) const { return keys_->find(key_id); }

    // always takes from the request bucket; a mutation also takes from the tighter one, whose view it answers
    [[nodiscard]] BucketView take_rate(const ApiKey& key, bool mutating);

private:
    struct Buckets {
        RateSpec spec{};
        TokenBucket requests;
        TokenBucket mutations;
    };

    [[nodiscard]] bool note_bad_auth(std::string_view ip, std::uint64_t now_ns);
    [[nodiscard]] Refusal refuse_bad_auth(std::string_view ip, std::uint64_t now_ns);

    const KeyDirectory* keys_;
    WallClock wall_;
    AuthPolicy policy_;
    std::mutex mutex_;
    std::map<std::string, ReplayGuard, std::less<>> replay_;
    std::map<std::string, Buckets, std::less<>> buckets_;
    std::unordered_map<std::string, TokenBucket> bad_auth_;
};

} // namespace raikiri::clientapi
