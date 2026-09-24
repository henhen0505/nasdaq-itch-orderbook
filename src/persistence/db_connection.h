#pragma once

#include "persistence/db_config.h"

#include <mysqlx/xdevapi.h>

#include <string>

namespace mde {

// Thin RAII wrapper around mysqlx::Session. Owns the session for its
// lifetime (closed on destruction) and exposes exactly the surface the
// writers in this module need: raw SQL execution for DDL/ad hoc statements,
// a parameterized-statement builder (.bind() pattern), and transaction
// control.
class DbConnection {
public:
    explicit DbConnection(const DbConfig& config);
    ~DbConnection();

    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    // Executes a raw SQL statement with no bound parameters (DDL, DROP
    // TABLE, etc).
    void execute(const std::string& sql_text);

    // Returns a statement for `sql_text` (containing `?` placeholders) for
    // the caller to bind parameters onto and execute, e.g.:
    //   conn.sql("INSERT INTO t (a) VALUES (?)").bind(value).execute();
    mysqlx::SqlStatement sql(const std::string& sql_text);

    void start_transaction();
    void commit();
    void rollback();

private:
    mysqlx::Session session_;
};

} // namespace mde
