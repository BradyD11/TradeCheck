#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace lob {

inline std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 1 ns buckets up to 1 ms: constant memory no matter how many samples (a full ITCH day is ~400M).
class LatencyHistogram {
 public:
  static constexpr std::uint64_t kMaxNs = 1'000'000;

  void record(std::uint64_t ns) {
    ++buckets_[std::min(ns, kMaxNs)];
    ++count_;
    sum_ += ns;
    max_ = std::max(max_, ns);
  }

  std::uint64_t percentile(double p) const {
    if (count_ == 0) return 0;
    auto target = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(p / 100.0 * count_)));
    std::uint64_t seen = 0;
    for (std::uint64_t i = 0; i <= kMaxNs; ++i) {
      seen += buckets_[i];
      if (seen >= target) return i;
    }
    return kMaxNs;
  }

  std::uint64_t count() const { return count_; }
  std::uint64_t max() const { return max_; }
  double mean() const { return count_ ? static_cast<double>(sum_) / count_ : 0; }

  void print(const char* label) const {
    std::printf("  %-14s n=%-11llu p50=%5llu  p99=%6llu  p99.9=%6llu  max=%8llu  mean=%7.1f ns\n", label,
                (unsigned long long)count_, (unsigned long long)percentile(50), (unsigned long long)percentile(99),
                (unsigned long long)percentile(99.9), (unsigned long long)max_, mean());
  }

 private:
  std::vector<std::uint64_t> buckets_ = std::vector<std::uint64_t>(kMaxNs + 1);
  std::uint64_t count_ = 0, sum_ = 0, max_ = 0;
};

}  // namespace lob
