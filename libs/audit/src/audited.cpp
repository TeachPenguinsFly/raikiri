#include <raikiri/audit/audited.hpp>

#include <glaze/glaze.hpp>
#include <string_view>

namespace raikiri::audit {

namespace internal {

constexpr std::size_t k_max_quoted_bytes = 768;

void quote_json(std::string& out, std::string_view key, std::string_view body) {
    if (body.empty()) {
        return;
    }
    out.append(",\"");
    out.append(key);
    if (body.size() <= k_max_quoted_bytes && body.find('\n') == std::string_view::npos && !glz::validate_json(body)) {
        out.append("\":");
        out.append(body);
    } else {
        out.append("_bytes\":");
        out.append(std::to_string(body.size()));
    }
}

} // namespace internal

http::Handler audited(http::Handler inner, Sink& sink, const Clock& clock, std::string actor, Classify classify) {
    return [inner = std::move(inner), &sink, &clock, actor = std::move(actor), classify = std::move(classify)](
               const http::Request& req) -> http::Response {
        const auto record = [&](const http::Response& answer, std::string_view outcome) {
            const auto command = classify(req, answer);
            if (!command.has_value()) {
                return;
            }
            std::string detail = "{\"status\":" + std::to_string(answer.status) + ",\"outcome\":\"";
            detail.append(outcome);
            detail.push_back('"');
            internal::quote_json(detail, "request", req.body);
            internal::quote_json(detail, "answer", answer.body);
            detail.push_back('}');
            sink.append({.ts = clock.wall(),
                         .actor = actor,
                         .action = command->action,
                         .subject = command->subject,
                         .detail_json = detail});
        };
        try {
            auto answer = inner(req);
            record(answer, answer.status >= 200 && answer.status < 300 ? "allowed" : "refused");
            return answer;
        } catch (...) {
            // the server answers this one with a 500; the attempt still happened
            record(http::Response{.status = 500, .body = {}}, "failed");
            throw;
        }
    };
}

} // namespace raikiri::audit
