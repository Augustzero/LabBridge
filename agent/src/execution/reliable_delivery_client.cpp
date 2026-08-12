#include "labbridge/agent/execution/reliable_delivery_client.h"

#include "labbridge/core/logging.h"

#include <algorithm>
#include <cstdint>
#include <sstream>

namespace labbridge::agent {
namespace {

constexpr std::string_view kComponent = "reliable-delivery";

bool retryable(const TaskExecutionClientError& error) {
    if (error.kind() == TaskExecutionErrorKind::Network ||
        error.kind() == TaskExecutionErrorKind::ServerError) {
        return true;
    }
    return error.kind() == TaskExecutionErrorKind::HttpStatus &&
           (error.http_status() == 408 || error.http_status() == 429 ||
            (error.http_status() >= 500 && error.http_status() <= 599));
}

std::string error_kind_name(TaskExecutionErrorKind kind) {
    switch (kind) {
        case TaskExecutionErrorKind::Network:
            return "network";
        case TaskExecutionErrorKind::HttpStatus:
            return "http_status";
        case TaskExecutionErrorKind::InvalidResponse:
            return "invalid_response";
        case TaskExecutionErrorKind::ServerError:
            return "server_error";
    }
    return "unknown";
}

std::uint64_t stable_hash(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

ReliableDeliveryClient::ReliableDeliveryClient(
    ITaskExecutionClient& client,
    AgentQueueStore& store,
    std::chrono::seconds retry_initial,
    std::chrono::seconds retry_max)
    : client_(client),
      store_(store),
      retry_initial_(retry_initial),
      retry_max_(retry_max) {
    if (retry_initial_ <= std::chrono::seconds::zero() ||
        retry_max_ < retry_initial_) {
        throw std::invalid_argument("invalid reliable delivery retry interval");
    }
}

std::chrono::milliseconds ReliableDeliveryClient::retry_delay(
    const std::string& key, int attempt) const {
    auto base = retry_initial_;
    for (int index = 1; index < attempt && base < retry_max_; ++index) {
        base = std::min(retry_max_, base * 2);
    }
    const auto base_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(base);
    const auto jitter = static_cast<long long>(
        stable_hash(key + "\n" + std::to_string(attempt)) % 201ULL);
    return base_ms + std::chrono::milliseconds{
                         base_ms.count() * jitter / 1000};
}

template <typename Result, typename Call>
Result ReliableDeliveryClient::deliver(
    const std::string& request_type,
    const std::string& idempotency_key,
    Call&& call) const {
    int attempt = store_.delivery_attempt_count(request_type, idempotency_key);
    if (attempt > 0) {
        const auto remaining = store_.delivery_retry_remaining(request_type, idempotency_key);
        std::unique_lock<std::mutex> lock{wait_mutex_};
        wait_condition_.wait_for(lock, remaining);
        store_.resume_delivery(request_type, idempotency_key);
    }
    while (!stop_requested_.load(std::memory_order_acquire)) {
        try {
            return call();
        } catch (const TaskExecutionClientError& error) {
            ++attempt;
            const bool should_retry = retryable(error);
            const auto delay = retry_delay(
                idempotency_key + "\n" + request_type, attempt);
            store_.record_delivery_failure(
                request_type, idempotency_key, should_retry,
                error_kind_name(error.kind()), error.http_status(),
                error.what(), delay);
            std::ostringstream message;
            message << "delivery failed; request_type=" << request_type
                    << "; attempt=" << attempt
                    << "; retryable=" << (should_retry ? "true" : "false")
                    << "; http_status=" << error.http_status();
            if (!should_retry) {
                labbridge::core::log_error(kComponent, message.str());
                throw DeliveryAbandoned{error.what()};
            }
            message << "; retry_in_ms=" << delay.count();
            labbridge::core::log_warn(kComponent, message.str());
            std::unique_lock<std::mutex> lock{wait_mutex_};
            wait_condition_.wait_for(lock, delay, [this] {
                return stop_requested_.load(std::memory_order_acquire);
            });
            if (!stop_requested_.load(std::memory_order_acquire)) {
                store_.resume_delivery(request_type, idempotency_key);
            }
        }
    }
    throw DeliveryAbandoned{"delivery stopped and remains pending"};
}

StartTaskRunResult ReliableDeliveryClient::start_task_run(
    const StartTaskRunRequest& request) const {
    return deliver<StartTaskRunResult>("start", request.execution_key, [&] {
        return client_.start_task_run(request);
    });
}

RawFileManifestResult ReliableDeliveryClient::report_raw_file_manifest(
    const RawFileManifestRequest& request) const {
    return deliver<RawFileManifestResult>("manifest", request.idempotency_key,
                                         [&] {
        return client_.report_raw_file_manifest(request);
    });
}

TaskRunReportResult ReliableDeliveryClient::report_task_run(
    const TaskRunReportRequest& request) const {
    return deliver<TaskRunReportResult>("report", request.idempotency_key, [&] {
        return client_.report_task_run(request);
    });
}

void ReliableDeliveryClient::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    wait_condition_.notify_all();
}

}  // namespace labbridge::agent
