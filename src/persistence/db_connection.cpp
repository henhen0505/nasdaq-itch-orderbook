#include "persistence/db_connection.h"

namespace mde {

DbConnection::DbConnection(const DbConfig& config)
    : session_(config.host, config.port, config.user, config.password, config.schema) {
}

DbConnection::~DbConnection() {
    // A destructor must never let an exception escape -- close() can throw
    // (e.g. on a dropped connection), so swallow it here. There is nothing
    // more useful to do with a close failure during teardown.
    try {
        session_.close();
    } catch (...) {
    }
}

void DbConnection::execute(const std::string& sql_text) {
    session_.sql(sql_text).execute();
}

mysqlx::SqlStatement DbConnection::sql(const std::string& sql_text) {
    return session_.sql(sql_text);
}

void DbConnection::start_transaction() {
    session_.startTransaction();
}

void DbConnection::commit() {
    session_.commit();
}

void DbConnection::rollback() {
    session_.rollback();
}

} // namespace mde
