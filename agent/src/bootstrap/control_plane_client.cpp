#include "labbridge/agent/bootstrap/control_plane_client.h"

#include "labbridge/core/logging.h"

#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace labbridge::agent {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;

constexpr std::string_view kHttpPrefix = "http://";
constexpr std::size_t kMaximumResponseBytes = 1024U * 1024U;

struct ParsedServerUrl {
    std::string host;
    std::string port;
    std::string host_header;
};

bool contains_whitespace(std::string_view value) {
    return std::any_of(
        value.begin(),
        value.end(),
        [](const unsigned char ch) { return std::isspace(ch); });
}

void validate_port(std::string_view port) {
    if (port.empty()) {
        throw std::invalid_argument("port must not be empty");
    }

    unsigned int number = 0;
    const auto* begin = port.data();
    const auto* end = begin + port.size();
    const auto [position, error] = std::from_chars(begin, end, number);
    if (error != std::errc{} || position != end || number == 0 || number > 65535) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
}

ParsedServerUrl parse_server_url(std::string_view server_url) {
    if (server_url.compare(0, kHttpPrefix.size(), kHttpPrefix) != 0) {
        throw std::invalid_argument(
            "only an http:// control plane URL is supported");
    }
    if (contains_whitespace(server_url)) {
        throw std::invalid_argument("URL must not contain whitespace");
    }

    auto authority = server_url.substr(kHttpPrefix.size());
    if (!authority.empty() && authority.back() == '/') {
        authority.remove_suffix(1);
    }
    if (authority.empty()) {
        throw std::invalid_argument("host is required");
    }
    if (authority.find_first_of("/?#@") != std::string_view::npos) {
        throw std::invalid_argument(
            "URL must contain only a host and optional port");
    }

    ParsedServerUrl result;
    result.host_header = std::string{authority};
    if (authority.front() == '[') {
        const auto closing_bracket = authority.find(']');
        if (closing_bracket == std::string_view::npos ||
            closing_bracket == 1) {
            throw std::invalid_argument("invalid bracketed IPv6 host");
        }
        result.host = std::string{authority.substr(1, closing_bracket - 1)};
        const auto suffix = authority.substr(closing_bracket + 1);
        if (suffix.empty()) {
            result.port = "80";
        } else {
            if (suffix.front() != ':') {
                throw std::invalid_argument("invalid bracketed IPv6 port");
            }
            result.port = std::string{suffix.substr(1)};
        }
    } else {
        const auto first_colon = authority.find(':');
        const auto last_colon = authority.rfind(':');
        if (first_colon != std::string_view::npos &&
            first_colon != last_colon) {
            throw std::invalid_argument("IPv6 hosts must use brackets");
        }
        if (last_colon == std::string_view::npos) {
            result.host = std::string{authority};
            result.port = "80";
        } else {
            result.host = std::string{authority.substr(0, last_colon)};
            result.port = std::string{authority.substr(last_colon + 1)};
        }
    }

    if (result.host.empty()) {
        throw std::invalid_argument("host is required");
    }
    validate_port(result.port);
    return result;
}

std::string encode_path_segment(std::string_view value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char ch : value) {
        const bool unreserved =
            std::isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[ch >> 4U]);
            encoded.push_back(kHex[ch & 0x0FU]);
        }
    }
    return encoded;
}

class HttpRequestOperation final
    : public std::enable_shared_from_this<HttpRequestOperation> {
public:
    HttpRequestOperation(asio::io_context& context,
                         std::string host,
                         std::string port,
                         http::request<http::string_body> request,
                         std::chrono::milliseconds timeout)
        : resolver_(context),
          stream_(context),
          timer_(context),
          host_(std::move(host)),
          port_(std::move(port)),
          request_(std::move(request)),
          timeout_(timeout) {
        parser_.body_limit(kMaximumResponseBytes);
    }

    void start() {
        timer_.expires_after(timeout_);
        timer_.async_wait(
            [self = shared_from_this()](const boost::system::error_code& error) {
                self->on_timeout(error);
            });
        resolver_.async_resolve(
            host_,
            port_,
            [self = shared_from_this()](
                const boost::system::error_code& error,
                const tcp::resolver::results_type& endpoints) {
                self->on_resolve(error, endpoints);
            });
    }

    http::response<http::string_body> take_response() {
        if (timed_out_) {
            throw ControlPlaneClientError{
                ControlPlaneErrorKind::Network,
                "control plane request timed out"};
        }
        if (error_) {
            throw ControlPlaneClientError{
                ControlPlaneErrorKind::Network,
                "control plane network failure: " + error_.message()};
        }
        return std::move(response_);
    }

private:
    void on_timeout(const boost::system::error_code& error) {
        if (error == asio::error::operation_aborted || finished_) {
            return;
        }
        timed_out_ = true;
        resolver_.cancel();
        boost::system::error_code ignored;
        stream_.socket().cancel(ignored);
        finish(asio::error::timed_out);
    }

    void on_resolve(const boost::system::error_code& error,
                    const tcp::resolver::results_type& endpoints) {
        if (finished_) {
            return;
        }
        if (error) {
            finish(error);
            return;
        }
        stream_.async_connect(
            endpoints,
            [self = shared_from_this()](
                const boost::system::error_code& connect_error,
                const tcp::endpoint&) {
                self->on_connect(connect_error);
            });
    }

    void on_connect(const boost::system::error_code& error) {
        if (finished_) {
            return;
        }
        if (error) {
            finish(error);
            return;
        }
        http::async_write(
            stream_,
            request_,
            [self = shared_from_this()](
                const boost::system::error_code& write_error,
                std::size_t) {
                self->on_write(write_error);
            });
    }

    void on_write(const boost::system::error_code& error) {
        if (finished_) {
            return;
        }
        if (error) {
            finish(error);
            return;
        }
        http::async_read(
            stream_,
            buffer_,
            parser_,
            [self = shared_from_this()](
                const boost::system::error_code& read_error,
                std::size_t) {
                self->on_read(read_error);
            });
    }

    void on_read(const boost::system::error_code& error) {
        if (finished_) {
            return;
        }
        if (error) {
            finish(error);
            return;
        }
        response_ = parser_.release();
        finish({});
    }

    void finish(const boost::system::error_code& error) {
        if (finished_) {
            return;
        }
        finished_ = true;
        error_ = error;
        boost::system::error_code ignored;
        timer_.cancel(ignored);
        stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
        stream_.socket().close(ignored);
    }

    tcp::resolver resolver_;
    beast::tcp_stream stream_;
    asio::steady_timer timer_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    http::response_parser<http::string_body> parser_;
    http::response<http::string_body> response_;
    std::string host_;
    std::string port_;
    std::chrono::milliseconds timeout_;
    boost::system::error_code error_;
    bool timed_out_{false};
    bool finished_{false};
};

std::optional<std::pair<std::string, std::string>> service_error(
    const Json& response) {
    if (!response.is_object() || !response.contains("ok") ||
        !response["ok"].is_boolean() || response["ok"].get<bool>() ||
        !response.contains("error") || !response["error"].is_object()) {
        return std::nullopt;
    }
    const auto& error = response["error"];
    if (!error.contains("code") || !error["code"].is_string() ||
        !error.contains("message") || !error["message"].is_string()) {
        return std::nullopt;
    }
    return std::pair{
        error["code"].get<std::string>(),
        error["message"].get<std::string>()};
}

Json success_data(unsigned int status, const std::string& body) {
    Json response;
    try {
        response = Json::parse(body);
    } catch (const Json::parse_error&) {
        if (status < 200 || status >= 300) {
            throw ControlPlaneClientError{
                ControlPlaneErrorKind::HttpStatus,
                "control plane returned HTTP status " + std::to_string(status),
                status};
        }
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidJson,
            "control plane returned invalid JSON",
            status};
    }

    if (const auto error = service_error(response); error.has_value()) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::ServerError,
            "control plane error " + error->first + ": " + error->second,
            status,
            error->first};
    }
    if (status < 200 || status >= 300) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::HttpStatus,
            "control plane returned HTTP status " + std::to_string(status),
            status};
    }
    if (!response.is_object() || !response.contains("ok") ||
        !response["ok"].is_boolean() || !response["ok"].get<bool>() ||
        !response.contains("data") || !response["data"].is_object()) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "control plane returned an invalid response envelope",
            status};
    }
    return response["data"];
}

std::string required_string(const Json& object,
                            std::string_view field,
                            unsigned int http_status) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_string()) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "control plane response field '" + std::string{field} +
                "' must be a string",
            http_status};
    }
    return iterator->get<std::string>();
}

bool required_boolean(const Json& object,
                      std::string_view field,
                      unsigned int http_status) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_boolean()) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "control plane response field '" + std::string{field} +
                "' must be a boolean",
            http_status};
    }
    return iterator->get<bool>();
}

std::vector<std::string> required_string_array(
    const Json& object,
    std::string_view field,
    unsigned int http_status) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_array()) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "control plane response field '" + std::string{field} +
                "' must be an array",
            http_status};
    }
    std::vector<std::string> values;
    values.reserve(iterator->size());
    for (const auto& value : *iterator) {
        if (!value.is_string()) {
            throw ControlPlaneClientError{
                ControlPlaneErrorKind::InvalidResponse,
                "control plane response field '" + std::string{field} +
                    "' entries must be strings",
                http_status};
        }
        values.push_back(value.get<std::string>());
    }
    return values;
}

std::string terminal_status(labbridge::core::TaskRunStatus status) {
    if (status == labbridge::core::TaskRunStatus::Succeeded) {
        return "succeeded";
    }
    if (status == labbridge::core::TaskRunStatus::Failed) {
        return "failed";
    }
    throw std::invalid_argument("task run report status must be terminal");
}

}  // namespace

ControlPlaneClientError::ControlPlaneClientError(
    ControlPlaneErrorKind kind,
    std::string message,
    unsigned int http_status,
    std::string server_code)
    : std::runtime_error(std::move(message)),
      kind_(kind),
      http_status_(http_status),
      server_code_(std::move(server_code)) {}

ControlPlaneErrorKind ControlPlaneClientError::kind() const noexcept {
    return kind_;
}

unsigned int ControlPlaneClientError::http_status() const noexcept {
    return http_status_;
}

const std::string& ControlPlaneClientError::server_code() const noexcept {
    return server_code_;
}

void validate_control_plane_url(std::string_view server_url) {
    static_cast<void>(parse_server_url(server_url));
}

ControlPlaneClient::ControlPlaneClient(
    std::string server_url,
    std::chrono::milliseconds request_timeout)
    : request_timeout_(request_timeout) {
    if (request_timeout_.count() <= 0) {
        throw std::invalid_argument("request timeout must be positive");
    }
    const auto parsed = parse_server_url(server_url);
    host_ = parsed.host;
    port_ = parsed.port;
    host_header_ = parsed.host_header;
}

void ControlPlaneClient::register_node(
    const labbridge::core::NodeInfo& node) const {
    Json body{
        {"node_code", node.node_code},
        {"name", node.name},
        {"agent_version", node.agent_version},
    };
    const auto response = request(
        "POST", "/api/v1/agents/register", body.dump());
    const auto data = success_data(response.status, response.body);
    if (required_string(data, "node_code", response.status) != node.node_code) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "registration response node_code does not match the request",
            response.status};
    }
    if (required_string(data, "status", response.status) != "offline") {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "registration response status must be offline",
            response.status};
    }
}

void ControlPlaneClient::send_heartbeat(
    const labbridge::core::NodeHeartbeat& heartbeat) const {
    Json body{
        {"node_code", heartbeat.node_code},
        {"agent_version", heartbeat.agent_version},
        {"reported_at", heartbeat.reported_at},
    };
    const auto response = request(
        "POST", "/api/v1/agents/heartbeat", body.dump());
    const auto data = success_data(response.status, response.body);
    if (required_string(data, "node_code", response.status) != heartbeat.node_code) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "heartbeat response node_code does not match the request",
            response.status};
    }
    if (required_string(data, "status", response.status) != "online") {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "heartbeat response status must be online",
            response.status};
    }
    if (required_string(data, "reported_at", response.status) !=
        heartbeat.reported_at) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "heartbeat response reported_at does not match the request",
            response.status};
    }
}

PulledAgentConfig ControlPlaneClient::fetch_config(
    const std::string& node_code) const {
    const auto response = request(
        "GET",
        "/api/v1/agents/" + encode_path_segment(node_code) + "/config");
    const auto data = success_data(response.status, response.body);
    if (!data.contains("node") || !data["node"].is_object() ||
        !data.contains("data_sources") ||
        !data["data_sources"].is_array() ||
        !data.contains("qc_rules") || !data["qc_rules"].is_array() ||
        !data.contains("tasks") || !data["tasks"].is_array()) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "control plane config must contain node, data_sources, "
            "qc_rules and tasks",
            response.status};
    }

    PulledAgentConfig result;
    const auto& node = data["node"];
    result.node.node_code = required_string(node, "node_code", response.status);
    result.node.name = required_string(node, "name", response.status);
    result.node.agent_version = required_string(node, "agent_version", response.status);
    result.last_heartbeat_at = required_string(node, "last_heartbeat_at", response.status);
    const auto status = required_string(node, "status", response.status);
    if (status == "online") {
        result.status = labbridge::core::NodeStatus::Online;
    } else if (status == "offline") {
        result.status = labbridge::core::NodeStatus::Offline;
    } else {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "control plane returned an unknown node status",
            response.status};
    }
    if (result.node.node_code != node_code) {
        throw ControlPlaneClientError{
            ControlPlaneErrorKind::InvalidResponse,
            "config response node_code does not match the request",
            response.status};
    }

    struct SourceProjection {
        std::string id;
        std::string node_code;
        std::string source_type;
        std::string name;
        std::string config_json;
    };
    struct QcProjection {
        std::string id;
        std::string task_id;
        std::string rule_type;
        std::string name;
        std::string config_json;
    };

    std::unordered_map<std::string, SourceProjection> sources;
    for (const auto& value : data["data_sources"]) {
        if (!value.is_object() || !value.contains("config") ||
            !value["config"].is_object()) {
            labbridge::core::log_warn(
                "control-plane-client",
                "ignoring invalid data source projection");
            continue;
        }
        try {
            SourceProjection source;
            source.id = required_string(value, "id", response.status);
            source.node_code =
                required_string(value, "node_code", response.status);
            source.source_type =
                required_string(value, "source_type", response.status);
            source.name = required_string(value, "name", response.status);
            source.config_json = value["config"].dump();
            sources[source.id] = std::move(source);
        } catch (const ControlPlaneClientError& error) {
            labbridge::core::log_warn(
                "control-plane-client",
                std::string{"ignoring invalid data source projection; reason="} +
                    error.what());
        }
    }

    std::unordered_map<std::string, QcProjection> rules;
    for (const auto& value : data["qc_rules"]) {
        if (!value.is_object() || !value.contains("config") ||
            !value["config"].is_object()) {
            labbridge::core::log_warn(
                "control-plane-client",
                "ignoring invalid QC rule projection");
            continue;
        }
        try {
            QcProjection rule;
            rule.id = required_string(value, "id", response.status);
            rule.task_id =
                required_string(value, "task_id", response.status);
            rule.rule_type =
                required_string(value, "rule_type", response.status);
            rule.name = required_string(value, "name", response.status);
            rule.config_json = value["config"].dump();
            rules[rule.task_id + "\n" + rule.id] = std::move(rule);
        } catch (const ControlPlaneClientError& error) {
            labbridge::core::log_warn(
                "control-plane-client",
                std::string{"ignoring invalid QC rule projection; reason="} +
                    error.what());
        }
    }

    for (const auto& value : data["tasks"]) {
        std::string task_id{"<unknown>"};
        try {
            if (!value.is_object()) {
                throw ControlPlaneClientError{
                    ControlPlaneErrorKind::InvalidResponse,
                    "control plane config task must be an object",
                    response.status};
            }

            labbridge::core::TaskConfig task;
            task.id = required_string(value, "id", response.status);
            task_id = task.id;
            task.node_code =
                required_string(value, "node_code", response.status);
            task.data_source_id =
                required_string(value, "data_source_id", response.status);
            task.name = required_string(value, "name", response.status);
            task.task_type =
                required_string(value, "task_type", response.status);
            task.schedule_expr =
                required_string(value, "schedule_expr", response.status);
            task.parser_type =
                required_string(value, "parser_type", response.status);
            task.qc_profile =
                required_string(value, "qc_profile", response.status);
            task.enabled =
                required_boolean(value, "enabled", response.status);
            if (!value.contains("qc_rule_ids") ||
                !value["qc_rule_ids"].is_array()) {
                throw ControlPlaneClientError{
                    ControlPlaneErrorKind::InvalidResponse,
                    "control plane task qc_rule_ids must be an array",
                    response.status};
            }
            if (task.node_code != node_code || !task.enabled) {
                throw ControlPlaneClientError{
                    ControlPlaneErrorKind::InvalidResponse,
                    "task is outside the requested enabled set",
                    response.status};
            }
            if (task.task_type != "local_file_import" ||
                task.parser_type != "csv_observation") {
                throw ControlPlaneClientError{
                    ControlPlaneErrorKind::InvalidResponse,
                    "task uses an unsupported task or parser type",
                    response.status};
            }

            const auto source = sources.find(task.data_source_id);
            if (source == sources.end() ||
                source->second.node_code != node_code ||
                source->second.source_type != "local_directory") {
                throw ControlPlaneClientError{
                    ControlPlaneErrorKind::InvalidResponse,
                    "task references an unavailable or unsupported data source",
                    response.status};
            }
            task.data_source = {
                source->second.id,
                source->second.node_code,
                labbridge::core::SourceType::LocalDirectory,
                source->second.name,
                source->second.config_json,
            };

            for (const auto& rule_id_value : value["qc_rule_ids"]) {
                if (!rule_id_value.is_string()) {
                    throw ControlPlaneClientError{
                        ControlPlaneErrorKind::InvalidResponse,
                        "task qc_rule_ids entries must be strings",
                        response.status};
                }
                const auto rule_id = rule_id_value.get<std::string>();
                const auto rule = rules.find(task.id + "\n" + rule_id);
                if (rule == rules.end() ||
                    (rule->second.rule_type != "required_fields" &&
                     rule->second.rule_type != "basic_timestamp_format")) {
                    throw ControlPlaneClientError{
                        ControlPlaneErrorKind::InvalidResponse,
                        "task references an unavailable or unsupported QC rule",
                        response.status};
                }
                task.qc_rules.push_back({
                    rule->second.id,
                    rule->second.rule_type,
                    rule->second.name,
                    rule->second.config_json,
                });
            }
            result.tasks.push_back(std::move(task));
        } catch (const ControlPlaneClientError& error) {
            labbridge::core::log_warn(
                "control-plane-client",
                "skipping task_id=" + task_id + "; reason=" + error.what());
        }
    }
    return result;
}

StartTaskRunResult ControlPlaneClient::start_task_run(
    const StartTaskRunRequest& request_value) const {
    Json body{
        {"node_code", request_value.node_code},
        {"task_id", request_value.task_id},
        {"execution_key", request_value.execution_key},
        {"scheduled_for", request_value.scheduled_for},
        {"started_at", request_value.started_at},
        {"trigger_type", request_value.trigger_type},
    };
    const auto response = request(
        "POST", "/api/v1/task-runs/start", body.dump());
    const auto data = success_data(response.status, response.body);
    return {
        required_string(data, "task_run_id", response.status),
        required_boolean(data, "replayed", response.status),
    };
}

RawFileManifestResult ControlPlaneClient::report_raw_file_manifest(
    const RawFileManifestRequest& request_value) const {
    Json files = Json::array();
    for (const auto& file : request_value.files) {
        files.push_back({
            {"original_name", file.original_name},
            {"file_hash", file.file_hash},
            {"storage_path", file.storage_path},
            {"size_bytes", file.size_bytes},
            {"source_mtime", file.source_mtime},
            {"ingest_status", file.ingest_status},
        });
    }
    Json body{
        {"task_run_id", request_value.task_run_id},
        {"node_code", request_value.node_code},
        {"idempotency_key", request_value.idempotency_key},
        {"files", std::move(files)},
    };
    const auto response = request(
        "POST", "/api/v1/raw-files/manifest", body.dump());
    const auto data = success_data(response.status, response.body);
    return {
        required_string_array(data, "raw_file_ids", response.status),
        required_boolean(data, "replayed", response.status),
    };
}

TaskRunReportResult ControlPlaneClient::report_task_run(
    const TaskRunReportRequest& request_value) const {
    Json records = Json::array();
    for (const auto& parsed : request_value.parsed_records) {
        Json qc_results = Json::array();
        for (const auto& qc : parsed.qc_results) {
            qc_results.push_back({
                {"qc_rule_id", qc.qc_rule_id},
                {"level", qc.level},
                {"result", qc.result},
                {"message", qc.message},
            });
        }
        records.push_back({
            {"raw_file_id", parsed.raw_file_id},
            {"station_code", parsed.record.station_code},
            {"device_code", parsed.record.device_code},
            {"record_time", parsed.record.record_time},
            {"payload_json", parsed.record.payload_json},
            {"parse_status", parsed.parse_status},
            {"qc_results", std::move(qc_results)},
        });
    }
    Json body{
        {"task_run_id", request_value.task_run_id},
        {"node_code", request_value.node_code},
        {"idempotency_key", request_value.idempotency_key},
        {"status", terminal_status(request_value.status)},
        {"finished_at", request_value.finished_at},
        {"items_total", request_value.items_total},
        {"items_success", request_value.items_success},
        {"items_failed", request_value.items_failed},
        {"error_summary", request_value.error_summary},
        {"parsed_records", std::move(records)},
    };
    const auto response = request(
        "POST", "/api/v1/task-runs/report", body.dump());
    const auto data = success_data(response.status, response.body);
    return {
        required_string_array(data, "parsed_record_ids", response.status),
        required_string_array(data, "qc_result_ids", response.status),
        required_string_array(data, "alert_ids", response.status),
        required_boolean(data, "replayed", response.status),
    };
}


ControlPlaneClient::HttpResponse ControlPlaneClient::request(
    std::string_view method,
    const std::string& target,
    std::string body) const {
    http::verb verb;
    if (method == "GET") {
        verb = http::verb::get;
    } else if (method == "POST") {
        verb = http::verb::post;
    } else {
        throw std::invalid_argument("unsupported HTTP method");
    }

    http::request<http::string_body> request{verb, target, 11};
    request.set(http::field::host, host_header_);
    request.set(http::field::user_agent, "LabBridge-Agent");
    request.set(http::field::accept, "application/json");
    request.set(http::field::connection, "close");
    if (verb == http::verb::post) {
        request.set(http::field::content_type, "application/json");
        request.body() = std::move(body);
        request.prepare_payload();
    }

    asio::io_context context;
    auto operation = std::make_shared<HttpRequestOperation>(
        context,
        host_,
        port_,
        std::move(request),
        request_timeout_);
    operation->start();
    context.run();
    auto response = operation->take_response();
    return {
        response.result_int(),
        std::move(response.body()),
    };
}

}  // namespace labbridge::agent
