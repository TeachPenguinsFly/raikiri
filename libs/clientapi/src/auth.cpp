#include <raikiri/clientapi/application/auth.hpp>

namespace raikiri::clientapi {

namespace {

constexpr std::size_t k_bad_auth_addresses = 65'536;

bool same_rate(const RateSpec& a, const RateSpec& b) noexcept {
    return a.requests_per_second == b.requests_per_second && a.burst == b.burst &&
           a.mutations_per_second == b.mutations_per_second && a.mutation_burst == b.mutation_burst;
}

} // namespace

bool Authenticator::note_bad_auth(std::string_view ip, std::uint64_t now_ns) {
    const std::scoped_lock lock{mutex_};
    if (bad_auth_.size() >= k_bad_auth_addresses) {
        bad_auth_.clear();
    }
    auto [at, fresh] = bad_auth_.try_emplace(std::string{ip},
                                             TokenBucket{policy_.bad_auth_per_second, policy_.bad_auth_burst, now_ns});
    return at->second.take(now_ns).allowed;
}

Refusal Authenticator::refuse_bad_auth(std::string_view ip, std::uint64_t now_ns) {
    if (!note_bad_auth(ip, now_ns)) {
        return {.status = 429,
                .code = "auth-failures",
                .title = "too many failed authentications from this address",
                .retry_after_s = 30};
    }
    return {.status = 401, .code = "auth-failed", .title = "unknown key or bad signature"};
}

Outcome<std::shared_ptr<const ApiKey>> Authenticator::check(const SignedRequest& request) {
    const auto now_ns = wall_().ns;
    const auto now_ms = now_ns / 1'000'000ULL;
    if (request.key_id.empty() || request.timestamp.empty() || request.signature.empty()) {
        return std::unexpected(refuse_bad_auth(request.peer_ip, now_ns));
    }
    const auto stamped = parse_millis(request.timestamp);
    if (!stamped.has_value() || *stamped + policy_.skew_ms < now_ms || *stamped > now_ms + policy_.skew_ms) {
        return std::unexpected(Refusal{.status = 401,
                                       .code = "timestamp-skew",
                                       .title = "timestamp outside the allowed window",
                                       .detail = "server time is " + std::to_string(now_ms) + " ms; the window is " +
                                                 std::to_string(policy_.skew_ms) + " ms either side"});
    }
    auto key = keys_->find(request.key_id);
    const auto message_ok = key != nullptr &&
                            keys_->verify(
                                *key,
                                canonical_request(
                                    request.timestamp, request.method, request.path, request.query, request.body),
                                request.signature);
    if (!message_ok) {
        return std::unexpected(refuse_bad_auth(request.peer_ip, now_ns));
    }
    {
        const std::scoped_lock lock{mutex_};
        auto guard = replay_.find(key->id);
        if (guard == replay_.end()) {
            // a signature is valid until its timestamp leaves the window, at most two windows from now
            guard = replay_.emplace(key->id, ReplayGuard{2 * policy_.skew_ms + 1}).first;
        }
        if (!guard->second.admit(request.signature, now_ms)) {
            return std::unexpected(
                Refusal{.status = 401, .code = "replayed", .title = "this signature was already used"});
        }
    }
    if (!key->ips.empty() && !in_any_range(key->ips, request.peer_ip)) {
        return std::unexpected(Refusal{.status = 403,
                                       .code = "ip-not-allowed",
                                       .title = "this key does not accept calls from this address",
                                       .detail = std::string{request.peer_ip}});
    }
    return key;
}

BucketView Authenticator::take_rate(const ApiKey& key, bool mutating) {
    const auto now_ns = wall_().ns;
    const std::scoped_lock lock{mutex_};
    auto found = buckets_.find(key.id);
    if (found != buckets_.end() && !same_rate(found->second.spec, key.rate)) {
        buckets_.erase(found);
        found = buckets_.end();
    }
    if (found == buckets_.end()) {
        found = buckets_
                    .emplace(
                        key.id,
                        Buckets{
                            .spec = key.rate,
                            .requests = TokenBucket{key.rate.requests_per_second, key.rate.burst, now_ns},
                            .mutations = TokenBucket{key.rate.mutations_per_second, key.rate.mutation_burst, now_ns}})
                    .first;
    }
    const auto general = found->second.requests.take(now_ns);
    if (!general.allowed || !mutating) {
        return general;
    }
    return found->second.mutations.take(now_ns);
}

} // namespace raikiri::clientapi
