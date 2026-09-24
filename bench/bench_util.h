#pragma once

#include <cmath>
#include <vector>

namespace bench {

inline double mean(const std::vector<double>& v) {
    double sum = 0.0;
    for (double x : v) {
        sum += x;
    }
    return sum / static_cast<double>(v.size());
}

// Sample standard deviation (n-1 denominator).
inline double sample_stddev(const std::vector<double>& v, double m) {
    if (v.size() < 2) {
        return 0.0;
    }
    double sum_sq = 0.0;
    for (double x : v) {
        sum_sq += (x - m) * (x - m);
    }
    return std::sqrt(sum_sq / static_cast<double>(v.size() - 1));
}

} // namespace bench
