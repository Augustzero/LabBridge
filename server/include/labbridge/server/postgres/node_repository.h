#pragma once

#include "labbridge/server/repositories/node_repository.h"
#include "labbridge/server/postgres/sql_session.h"

namespace labbridge::server {

class PostgresNodeRepository final : public INodeRepository {
public:
    explicit PostgresNodeRepository(ISqlSession& session);

    void upsert(NodeRecord node) override;
    std::optional<NodeRecord> find_by_code(const std::string& node_code) const override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
