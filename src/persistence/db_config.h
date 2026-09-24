#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace mde {

// Connection parameters for the MySQL X DevAPI session. Populated from
// environment variables so a future target (e.g. AWS RDS) can swap
// host/port/user/schema without touching code. Defaults match the local
// development setup.
struct DbConfig {
    std::string host;
    int port;
    std::string user;
    std::string schema;
    std::string password;

    // Reads MDE_DB_HOST / MDE_DB_PORT / MDE_DB_USER / MDE_DB_SCHEMA /
    // MDE_DB_PASSWORD from the environment. MDE_DB_PASSWORD has no default
    // and is required -- throws std::runtime_error with a clear message if
    // it is unset.
    static DbConfig from_env();
};

namespace detail {

inline std::string env_or_default(const char* name, const std::string& default_value) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : default_value;
}

} // namespace detail

inline DbConfig DbConfig::from_env() {
    DbConfig config;
    config.host = detail::env_or_default("MDE_DB_HOST", "localhost");
    config.port = std::stoi(detail::env_or_default("MDE_DB_PORT", "33060"));
    config.user = detail::env_or_default("MDE_DB_USER", "mde_user");
    config.schema = detail::env_or_default("MDE_DB_SCHEMA", "market_data_engine");

    const char* password = std::getenv("MDE_DB_PASSWORD");
    if (!password) {
        throw std::runtime_error(
            "DbConfig::from_env: environment variable MDE_DB_PASSWORD is not set.\n"
            "       Set it before running:\n"
            "         PowerShell:  $env:MDE_DB_PASSWORD = \"<password>\"\n"
            "         cmd.exe:     set MDE_DB_PASSWORD=<password>\n"
            "       See SETUP.md for details.");
    }
    config.password = password;

    return config;
}

} // namespace mde
