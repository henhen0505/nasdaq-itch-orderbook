#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mde {

inline std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("read_file: failed to open '" + path + "'");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// Strips `--` line comments before statement-splitting. Without this, a
// `;` inside a comment is indistinguishable from a real statement terminator
// to a naive split. Safe for DDL/DML files with no `--` inside quoted strings.
inline std::string strip_line_comments(const std::string& sql_text) {
    std::string result;
    std::istringstream stream(sql_text);
    std::string line;
    while (std::getline(stream, line)) {
        size_t comment_pos = line.find("--");
        if (comment_pos != std::string::npos) {
            line.erase(comment_pos);
        }
        result += line;
        result += '\n';
    }
    return result;
}

// Splits a `;`-delimited SQL script into individual statements (the X
// Protocol only accepts one statement per session.sql() call).
inline std::vector<std::string> split_statements(const std::string& sql_text) {
    std::vector<std::string> statements;
    std::stringstream stream(strip_line_comments(sql_text));
    std::string statement;
    while (std::getline(stream, statement, ';')) {
        if (statement.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        statements.push_back(statement);
    }
    return statements;
}

} // namespace mde
