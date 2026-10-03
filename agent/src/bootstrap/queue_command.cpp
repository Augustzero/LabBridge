#include "labbridge/agent/bootstrap/queue_command.h"

#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/agent/storage/agent_queue_store.h"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace labbridge::agent {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

void print_usage(std::ostream& out) {
    out << "usage:\n"
        << "  labbridge_agent <config.yaml>                                     "
           "run the agent\n"
        << "  labbridge_agent queue list --config <agent.yaml>                  "
           "list jobs waiting for attention\n"
        << "  labbridge_agent queue show <execution_key> --config <agent.yaml>  "
           "show details of one attention job\n"
        << "  labbridge_agent queue retry <execution_key> --config <agent.yaml> "
           "allow one attention job to resume\n"
        << "\n"
        << "queue commands only read agent.node_code and storage.queue_db.\n"
        << "retry sends nothing: view the reason, stop the agent, fix the "
           "external cause,\n"
        << "run retry, then start the agent again.\n";
}

struct QueueCommandArgs {
    std::string verb;
    std::string execution_key;
    std::string config_path;
};

enum class ParseOutcome { Parsed, HelpShown, Invalid };

// 解析 queue 之后的参数：动词、可选执行键和必需的 --config。
// 用法错误由调用方统一输出 usage 并返回退出码 2。
ParseOutcome parse_args(int argc, char* argv[], QueueCommandArgs* args) {
    for (int index = 0; index < argc; ++index) {
        const std::string token{argv[index]};
        if (token == "-h" || token == "--help" || token == "help") {
            return ParseOutcome::HelpShown;
        }
        if (token == "--config") {
            if (index + 1 >= argc || !args->config_path.empty()) {
                return ParseOutcome::Invalid;
            }
            args->config_path = argv[++index];
            continue;
        }
        if (token.rfind("--", 0) == 0) {
            return ParseOutcome::Invalid;
        }
        if (args->verb.empty()) {
            args->verb = token;
        } else if (args->execution_key.empty()) {
            args->execution_key = token;
        } else {
            return ParseOutcome::Invalid;
        }
    }

    const bool needs_key =
        args->verb == "show" || args->verb == "retry";
    if (args->verb != "list" && !needs_key) {
        return ParseOutcome::Invalid;
    }
    if (needs_key == args->execution_key.empty()) {
        return ParseOutcome::Invalid;
    }
    if (args->config_path.empty()) {
        return ParseOutcome::Invalid;
    }
    return ParseOutcome::Parsed;
}

// 详情输出的标签列宽，固定对齐方便肉眼扫（最长标签是 "  resume_stage:"）。
void print_field(const std::string& label, const std::string& value) {
    std::cout << std::left << std::setw(16) << label << value << '\n';
}

int run_list(const AgentQueueCommandConfig& config) {
    // list/show 只读连接，Agent 运行中也能在线查。
    const AgentQueueMaintenance store{config.queue_db, config.node_code, true};
    const auto jobs = store.list_attention_jobs();
    if (jobs.empty()) {
        std::cout << "no jobs waiting for attention\n";
        return kExitOk;
    }

    std::cout << jobs.size() << " job(s) waiting for attention:\n";
    for (const auto& job : jobs) {
        std::cout << job.execution_key << '\n';
        print_field("  task:", job.task_id);
        print_field("  error_kind:", job.error_kind);
        print_field("  updated_at:", job.updated_at);
        print_field("  reason:", job.reason);
    }
    return kExitOk;
}

int run_show(const AgentQueueCommandConfig& config,
             const std::string& execution_key) {
    const AgentQueueMaintenance store{config.queue_db, config.node_code, true};
    const auto detail = store.load_attention_job(execution_key);

    std::cout << detail.execution_key << '\n';
    print_field("  task:", detail.task_id);
    print_field("  stage:", "requires_attention");
    print_field("  resume_stage:", detail.resume_stage);
    print_field("  error_kind:", detail.error_kind);
    print_field("  reason:", detail.reason);
    print_field("  attempts:", std::to_string(detail.attempt_count));
    print_field("  started_at:", detail.started_at);
    print_field("  updated_at:", detail.updated_at);

    std::cout << "  files (" << detail.files.size() << "):\n";
    for (const auto& file : detail.files) {
        std::cout << "    [" << file.ordinal << "] "
                  << file.source_path << " (" << file.size_bytes
                  << " bytes)\n"
                  << "        archive: " << file.archive_path << " ["
                  << file.archive_state << "]\n";
    }

    std::cout << "  deliveries:\n";
    std::cout << std::left << "    " << std::setw(9) << "type"
              << std::setw(11) << "attempts" << std::setw(6) << "http"
              << std::setw(22) << "error_kind"
              << std::setw(13) << "body_bytes" << "next_attempt\n";
    for (const auto& delivery : detail.deliveries) {
        const auto http_status = delivery.last_http_status == 0
            ? "-"
            : std::to_string(delivery.last_http_status);
        std::cout << "    " << std::setw(9) << delivery.request_type
                  << std::setw(11) << delivery.attempt_count << std::setw(6)
                  << http_status << std::setw(22)
                  << (delivery.last_error_kind.empty()
                          ? "-"
                          : delivery.last_error_kind)
                  << std::setw(13) << delivery.body_bytes
                  << (delivery.next_attempt_at.empty()
                          ? "-"
                          : delivery.next_attempt_at)
                  << '\n';
        if (!delivery.last_error.empty()) {
            print_field("      last_error:", delivery.last_error);
        }
    }
    return kExitOk;
}

int run_retry(const AgentQueueCommandConfig& config,
              const std::string& execution_key) {
    // 先打开校验再抢锁：库路径不对时不留下 .lock 之类的副作用文件。
    // 维护连接本身不写任何东西，真正的修改都发生在拿到锁之后。
    AgentQueueMaintenance store{config.queue_db, config.node_code, false};
    // 运行中的 Agent 持有同一把 flock 直到退出；抢不到说明它还活着。
    AgentQueueLock lock{config.queue_db};
    const auto result = store.retry_attention_job(execution_key);

    if (result.status == QueueRetryStatus::NotAttention) {
        std::cout << "job " << execution_key
                  << " is not waiting for attention (current stage: "
                  << result.stage << "); nothing was changed\n";
        return kExitFailure;
    }
    std::cout << "job " << execution_key
              << " is allowed to resume at stage '" << result.stage << "'\n"
              << "start the agent to continue this job; allowing the retry "
                 "does not mean the delivery has succeeded\n";
    return kExitOk;
}

}  // namespace

int run_queue_command(int argc, char* argv[]) {
    QueueCommandArgs args;
    switch (parse_args(argc, argv, &args)) {
        case ParseOutcome::HelpShown:
            print_usage(std::cout);
            return kExitOk;
        case ParseOutcome::Invalid:
            std::cerr << "invalid queue command arguments\n";
            print_usage(std::cerr);
            return kExitUsage;
        case ParseOutcome::Parsed:
            break;
    }

    try {
        const auto config = load_agent_queue_config(args.config_path);
        if (args.verb == "list") {
            return run_list(config);
        }
        if (args.verb == "show") {
            return run_show(config, args.execution_key);
        }
        return run_retry(config, args.execution_key);
    } catch (const std::exception& error) {
        std::cerr << "queue " << args.verb
                  << " failed: " << error.what() << '\n';
        return kExitFailure;
    }
}

}  // namespace labbridge::agent
