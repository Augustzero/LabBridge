#pragma once

#include <string>
#include <utility>

namespace labbridge::core {

struct Status {
    bool ok{true};
    std::string message;

    static Status success() {
        return {true, {}};
    }

    static Status failure(std::string reason) {
        return {false, std::move(reason)};
    }
};

}  // namespace labbridge::core
