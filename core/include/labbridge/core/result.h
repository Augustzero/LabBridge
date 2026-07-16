#pragma once

#include <string>
#include <utility>

namespace labbridge::core {

enum class StatusCode {
    Ok,
    InvalidArgument,
    NotFound,
    Conflict,
};

struct Status {
    bool ok{true};
    std::string message;
    StatusCode code{StatusCode::Ok};

    Status() = default;

    Status(bool is_ok, std::string reason)
        : Status(is_ok,
                 std::move(reason),
                 is_ok ? StatusCode::Ok : StatusCode::InvalidArgument) {}

    Status(bool is_ok, std::string reason, StatusCode status_code)
        : ok(is_ok), message(std::move(reason)), code(status_code) {}

    static Status success() {
        return {true, {}, StatusCode::Ok};
    }

    static Status failure(std::string reason) {
        return {false, std::move(reason), StatusCode::InvalidArgument};
    }

    static Status failure(StatusCode code, std::string reason) {
        return {false, std::move(reason), code};
    }
};

}  // namespace labbridge::core
