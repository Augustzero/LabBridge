#include "labbridge/server/postgres/task_run_executor.h"

#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
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
    PostgresNodeRepository node_repository{session};
    PostgresQcRepository qc_repository{session};
    PostgresResultRepository result_repository{session};
    TaskRunService service{config_repository, task_run_repository,
                           node_repository, qc_repository,
                           result_repository};

    auto result = service.start(request);
    if (result.status.ok) {
        transaction.commit();
    }
    return result;
}

}  // namespace labbridge::server
