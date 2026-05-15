#include "labbridge/core/logging.h"
#include "labbridge/core/version.h"

#include <string>

namespace {

constexpr std::string_view kComponent = "agent";

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : "deploy/env/agent.example.yaml";

    labbridge::core::log_info(kComponent, "starting LabBridge agent");
    labbridge::core::log_info(kComponent, labbridge::core::kVersion);
    labbridge::core::log_info(kComponent, "config path: " + config_path);
    labbridge::core::log_info(kComponent, "phase 0 skeleton is ready");

    return 0;
}

