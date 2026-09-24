// Parser and order-book throughput benchmark. No database, no indexes
// involved -- pure in-memory work against the real ~274MB decompressed
// ITCH sample (data/itch_sample_12302019_100mb.bin). That file is a
// truncated download (HTTP range request against a larger source file) --
// parse_file() throws BufferUnderrunError at the exact truncation point,
// an expected stop, not a crash; caught and treated as end-of-measurement
// below.
//
// 1. Parser throughput: parse_file() (src/parser/itch_parser.h) with a
//    no-op callback (just counts messages) -- no order-book updates, no DB
//    writes. Reports MB/sec and messages/sec.
// 2. Order-book throughput: the same parsed stream additionally driven
//    through BookManager::process() (src/orderbook/book_manager.h) -- still
//    no DB writes. Reports messages/sec, and the delta versus parser-only
//    throughput (how much time the order-book logic itself adds on top of
//    parsing).
//
// Each measurement is run 3 times and reported as mean (+ sample stddev),
// same repeated-run discipline as Part A -- a single run doesn't establish
// a number is representative, even for deterministic in-memory work.

#include "bench_util.h"
#include "orderbook/book_manager.h"
#include "parser/binary_reader.h"
#include "parser/itch_parser.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MDE_PROJECT_ROOT
#error "MDE_PROJECT_ROOT must be defined by the build -- see CMakeLists.txt"
#endif

using namespace mde;

namespace {

constexpr size_t kRunsPerMeasurement = 3;
constexpr const char* kSampleRelativePath = "/data/itch_sample_12302019_100mb.bin";

struct RunResult {
    double elapsed_seconds;
    size_t message_count;
};

// data/itch_sample_12302019_100mb.bin is a truncated download (an HTTP
// range request against a much larger file)
// -- parse_file() is expected to throw BufferUnderrunError at the exact
// truncation point, a clean stop (the sample is a partial range download), not
// a crash or a bug in this benchmark. Caught here and treated as normal
// end-of-measurement: elapsed time and message count up to that point are
// what gets reported, same as every prior stage's throwaway driver did.
template <typename Fn>
RunResult run_and_time(const std::string& path, size_t& count, Fn&& parse_call) {
    auto start = std::chrono::steady_clock::now();
    try {
        parse_call();
    } catch (const BufferUnderrunError&) {
        // Expected: the sample file is truncated mid-frame at EOF.
    }
    std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    return {elapsed.count(), count};
}

// Parser-only: no order-book updates, no DB writes -- the callback just
// counts messages.
RunResult run_parser_only(const std::string& path) {
    size_t count = 0;
    return run_and_time(path, count, [&]() {
        parse_file(path, [&count](const ItchMessage&) { ++count; });
    });
}

// Parser + order-book state updates, still no DB writes.
RunResult run_parser_plus_book(const std::string& path) {
    BookManager manager;
    size_t count = 0;
    return run_and_time(path, count, [&]() {
        parse_file(path, [&manager, &count](const ItchMessage& msg) {
            ++count;
            manager.process(msg);
        });
    });
}

struct Summary {
    std::string name;
    std::vector<double> elapsed_seconds;
    size_t message_count = 0; // same across runs -- deterministic parse
};

Summary run_n_times(const std::string& name, const std::string& path, size_t n,
                     const std::function<RunResult(const std::string&)>& run_once) {
    Summary summary{name, {}, 0};
    for (size_t i = 0; i < n; ++i) {
        RunResult r = run_once(path);
        if (r.elapsed_seconds <= 0.0) {
            throw std::runtime_error(name + ": measured elapsed time was non-positive -- clock resolution issue");
        }
        summary.elapsed_seconds.push_back(r.elapsed_seconds);
        summary.message_count = r.message_count;
    }
    return summary;
}

void print_summary(const Summary& s, double file_size_mb) {
    double m = bench::mean(s.elapsed_seconds);
    double sd = bench::sample_stddev(s.elapsed_seconds, m);
    double mb_per_sec = file_size_mb / m;
    double msgs_per_sec = static_cast<double>(s.message_count) / m;

    std::cout << s.name << ": mean=" << m << "s, stddev=" << sd << "s, runs=" << s.elapsed_seconds.size() << "\n";
    std::cout << "  " << mb_per_sec << " MB/sec, " << msgs_per_sec << " messages/sec (" << s.message_count
              << " messages retained per run)\n";
    for (size_t i = 0; i < s.elapsed_seconds.size(); ++i) {
        std::cout << "  run " << (i + 1) << ": " << s.elapsed_seconds[i] << "s\n";
    }
}

} // namespace

int main() {
    try {
        const std::string sample_path = std::string(MDE_PROJECT_ROOT) + kSampleRelativePath;

        if (!std::filesystem::exists(sample_path)) {
            std::cerr << "[FAIL] sample file not found: " << sample_path << "\n";
            return 1;
        }

        uintmax_t file_size_bytes = std::filesystem::file_size(sample_path);
        double file_size_mb = static_cast<double>(file_size_bytes) / (1000.0 * 1000.0);

        std::cout << "Sample file: " << sample_path << " (" << file_size_bytes << " bytes, " << file_size_mb
                  << " MB)\n\n";

        Summary parser_only = run_n_times("parser_only", sample_path, kRunsPerMeasurement, run_parser_only);
        Summary parser_plus_book =
            run_n_times("parser_plus_orderbook", sample_path, kRunsPerMeasurement, run_parser_plus_book);

        std::cout << "=== Results ===\n";
        print_summary(parser_only, file_size_mb);
        print_summary(parser_plus_book, file_size_mb);

        double parser_only_mean = bench::mean(parser_only.elapsed_seconds);
        double parser_plus_book_mean = bench::mean(parser_plus_book.elapsed_seconds);
        double delta_seconds = parser_plus_book_mean - parser_only_mean;
        double delta_pct = (delta_seconds / parser_only_mean) * 100.0;

        std::cout << "\nOrder-book overhead on top of parsing: " << delta_seconds << "s (" << delta_pct
                  << "% of parser-only time)\n";

        if (parser_only.message_count != parser_plus_book.message_count) {
            std::cerr << "[FAIL] message counts differ between parser-only and parser+orderbook runs ("
                       << parser_only.message_count << " vs. " << parser_plus_book.message_count
                       << ") -- something is wrong with the measurement, not just the timing\n";
            return 1;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
