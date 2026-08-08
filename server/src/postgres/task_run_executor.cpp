#include "labbridge/server/postgres/task_run_executor.h"

#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/sql_transaction.h"
#include "labbridge/server/postgres/task_run_repository.h"

#include <utility>

namespace labbridge::server {

PostgresTaskRunExecutor::PostgresTaskRunExecutor(std::string connection_info)
    : connection_info_(std::move(connection_info)) {}

TaskRunCreateResult PostgresTaskRunExecutor::start(
    const StartTaskRunRequest& request) const {
    LibpqSqlSession session{connection_info_};
    SqlTransaction transaction{session};
    PostgresConfigRepository config_repository{session};
    PostgresTaskRunRepository task_run_repository{session};
    TaskRunService service{config_repository, task_run_repository};

    auto result = service.start(request);
    if (result.status.ok) {
        transaction.commit();
    }
    return result;
}

}  // namespace labbridge::server
