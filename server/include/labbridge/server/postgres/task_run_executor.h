#pragma once

#include "labbridge/server/application/task_run_service.h"

#include <string>

namespace labbridge::server {

class PostgresTaskRunExecutor {
public:
    explicit PostgresTaskRunExecutor(std::string connection_info);

    TaskRunCreateResult start(const StartTaskRunRequest& request) const;

private:
    std::string connection_info_;
};

}  // namespace labbridge::server
