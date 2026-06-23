#include "labbridge/core/logging.h"
#include "labbridge/core/version.h"

#include <string>

namespace {

constexpr std::string_view kComponent = "server";

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : "deploy/env/server.example.yaml";

    labbridge::core::log_info(kComponent, "starting LabBridge control plane");
    labbridge::core::log_info(kComponent, labbridge::core::kVersion);
    labbridge::core::log_info(kComponent, "config path: " + config_path);
    labbridge::core::log_info(kComponent, "control plane bootstrap is ready");

    return 0;
}
