#pragma once

#include <mysqlx/xdevapi.h>

#include <optional>
#include <string>

namespace mde {

inline mysqlx::Value to_value(char c) {
    return mysqlx::Value(std::string(1, c));
}

inline mysqlx::Value to_value(uint64_t v) {
    return mysqlx::Value(v);
}

inline mysqlx::Value to_value(const std::optional<char>& c) {
    return c ? mysqlx::Value(std::string(1, *c)) : mysqlx::Value();
}

template <typename T>
mysqlx::Value to_value(const std::optional<T>& v) {
    return v ? mysqlx::Value(*v) : mysqlx::Value();
}

} // namespace mde
