#pragma once

#include "labbridge/core/models.h"

#include <vector>

namespace labbridge::agent {

class IRuntimeConfigSink {
public:
    virtual ~IRuntimeConfigSink() = default;
    virtual void replace_config(
        std::vector<labbridge::core::TaskConfig> tasks) = 0;
};

}  // namespace labbridge::agent
