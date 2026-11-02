#pragma once

// the /api/v1 surface, schema/client-api.yaml; called on worker threads

#include <raikiri/clientapi/application/auth.hpp>
#include <raikiri/clientapi/application/desk.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/application/ports.hpp>
#include <raikiri/clientapi/application/refusal.hpp>
#include <raikiri/common/ulid.hpp>

#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::clientapi {

inline constexpr std::string_view k_api_prefix = "/api/v1/";
inline constexpr std::string_view k_stream_path = "/api/v1/stream";
inline constexpr std::string_view k_key_header = "X-Raikiri-Key";
inline constexpr std::string_view k_timestamp_header = "X-Raikiri-Timestamp";
inline constexpr std::string_view k_signature_header = "X-Raikiri-Signature";

struct EdgeRequest {
    std::string_view method{};
    std::string_view path{};
    std::string_view query{};
    std::string_view body{};
    std::string_view key_id{};
    std::string_view timestamp{};
    std::string_view signature{};
    std::string_view idempotency_key{};
    std::string_view peer_ip{};
};

struct EdgeResponse {
    int status{200};
    std::string content_type{"application/json"};
    std::string body{};
    std::vector<std::pair<std::string, std::string>> headers{};
};

class ApiRouter {
public:
    ApiRouter(Authenticator& auth,
              ClientDesk& desk,
              StreamHub& hub,
              AuditSink& audit,
              ClientBook& book,
              WallClock wall,
              ExtensionClient* extension_client = nullptr,
              std::vector<Extension> extensions = {});

    [[nodiscard]] EdgeResponse handle(const EdgeRequest& request);

    // a signed GET of the stream path; on success one stream slot is held until the session gives it back
    [[nodiscard]] std::expected<std::shared_ptr<const ApiKey>, EdgeResponse>
    authorize_stream(const EdgeRequest& request);

    // a market subscribe costs a request, and every instrument must be visible to the key
    [[nodiscard]] Outcome<void> check_market(const ApiKey& key, const std::vector<std::string>& instruments);

    [[nodiscard]] std::shared_ptr<const ApiKey> current_key(std::string_view key_id) const {
        return auth_->current(key_id);
    }

    void audit_stream(const ApiKey& key, std::string_view action, std::string_view subject, std::string_view ip);

private:
    struct Call;
    using Reply = Outcome<std::pair<int, std::string>>;
    using Handler = Reply (ApiRouter::*)(Call&);

    struct Route {
        std::string_view method;
        std::string_view pattern;
        Permission permission;
        std::string_view action; // audited when not empty; those are the mutations
        Handler handler;
    };

    [[nodiscard]] std::string next_request_id();
    [[nodiscard]] EdgeResponse render(const Refusal& refusal, std::string_view request_id) const;
    void audit(std::string_view actor,
               std::string_view action,
               std::string_view subject,
               int status,
               std::string_view code,
               std::string_view request_id,
               std::string_view ip);
    [[nodiscard]] Reply run_mutation(Call& call, const Route& route);
    [[nodiscard]] EdgeResponse handle_extension(const EdgeRequest& request,
                                                const ApiKey& key,
                                                const BucketView& rate,
                                                std::string_view tail,
                                                std::string_view request_id);
    [[nodiscard]] Reply call_extension(const Extension& extension,
                                       const ApiKey& key,
                                       const EdgeRequest& request,
                                       std::string_view path,
                                       std::string_view wildcard_base);

    Reply whoami(Call& call);
    Reply accounts(Call& call);
    Reply balances(Call& call);
    Reply positions(Call& call);
    Reply fills(Call& call);
    Reply instruments(Call& call);
    Reply instrument(Call& call);
    Reply markets(Call& call);
    Reply option_underlyings(Call& call);
    Reply option_chain(Call& call);
    Reply option_surface(Call& call);
    Reply list_orders(Call& call);
    Reply place_order(Call& call);
    Reply cancel_all(Call& call);
    Reply get_order(Call& call);
    Reply order_events(Call& call);
    Reply cancel_order(Call& call);
    Reply amend_order(Call& call);
    Reply list_algos(Call& call);
    Reply start_algo(Call& call);
    Reply get_algo(Call& call);
    Reply cancel_algo(Call& call);
    Reply pause_algo(Call& call);
    Reply resume_algo(Call& call);

    Authenticator* auth_;
    ClientDesk* desk_;
    StreamHub* hub_;
    AuditSink* audit_;
    ClientBook* book_;
    WallClock wall_;
    ExtensionClient* extension_client_;
    std::vector<Extension> extensions_;
    std::vector<Route> routes_;
    std::mutex ids_mutex_;
    UlidGen ids_;
};

} // namespace raikiri::clientapi
