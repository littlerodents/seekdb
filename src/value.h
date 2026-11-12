// Value model for MiniVectorDB.
//
// The engine only needs three column types (BIGINT, VARCHAR, VECTOR of float32), so a
// single tagged union keeps row storage and expression evaluation simple.  BIGINT is
// carried as int64 and never converted through double: the protocol requires exact
// integer round-trip.
#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace mvdb {

enum class ColType : uint8_t { BIGINT = 1, VARCHAR = 2, VECTOR = 3, DOUBLE = 4 };

struct Value {
  ColType type = ColType::BIGINT;
  bool is_null = false;
  int64_t i = 0;
  double d = 0.0;             // DOUBLE payload (computed distances)
  std::string s;              // VARCHAR payload, UTF-8
  std::vector<float> v;       // VECTOR payload

  Value() = default;
  static Value make_bigint(int64_t x) {
    Value val; val.type = ColType::BIGINT; val.i = x; return val;
  }
  static Value make_varchar(std::string str) {
    Value val; val.type = ColType::VARCHAR; val.s = std::move(str); return val;
  }
  static Value make_vector(std::vector<float> vec) {
    Value val; val.type = ColType::VECTOR; val.v = std::move(vec); return val;
  }
  static Value make_double(double x) {
    Value val; val.type = ColType::DOUBLE; val.d = x; return val;
  }
  void reset() {
    type = ColType::BIGINT;
    is_null = false;
    i = 0;
    d = 0.0;
    s.clear();
    v.clear();
  }
};

// Unicode code point count, which is what VARCHAR(n) measures.
inline int64_t utf8_length(const std::string &s) {
  int64_t count = 0;
  for (size_t k = 0; k < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[k]);
    size_t adv = 1;
    if ((c & 0x80) == 0) {
      adv = 1;
    } else if ((c & 0xE0) == 0xC0) {
      adv = 2;
    } else if ((c & 0xF0) == 0xE0) {
      adv = 3;
    } else if ((c & 0xF8) == 0xF0) {
      adv = 4;
    }
    k += adv;
    ++count;
  }
  return count;
}

// Byte-wise comparison equals code point order for valid UTF-8, which is the documented
// collation for VARCHAR in this task (no locale-aware ordering).
inline int compare_varchar(const std::string &a, const std::string &b) {
  const int cmp = std::memcmp(a.data(), b.data(), a.size() < b.size() ? a.size() : b.size());
  if (cmp != 0) {
    return cmp < 0 ? -1 : 1;
  }
  if (a.size() == b.size()) {
    return 0;
  }
  return a.size() < b.size() ? -1 : 1;
}

// Squared L2 distance for ranking, plus the square root only for the few rows that are
// actually returned: ordering by the squared value is identical, so the expensive sqrt is
// paid O(K) instead of O(N) on a full scan.
inline double l2_distance_sq(const std::vector<float> &a, const std::vector<float> &b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  // Four independent accumulators let the compiler keep the loop vectorized and hide the
  // latency of the dependent add.
  double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
  size_t k = 0;
  for (; k + 4 <= n; k += 4) {
    const double d0 = static_cast<double>(a[k]) - static_cast<double>(b[k]);
    const double d1 = static_cast<double>(a[k + 1]) - static_cast<double>(b[k + 1]);
    const double d2 = static_cast<double>(a[k + 2]) - static_cast<double>(b[k + 2]);
    const double d3 = static_cast<double>(a[k + 3]) - static_cast<double>(b[k + 3]);
    s0 += d0 * d0;
    s1 += d1 * d1;
    s2 += d2 * d2;
    s3 += d3 * d3;
  }
  for (; k < n; ++k) {
    const double d = static_cast<double>(a[k]) - static_cast<double>(b[k]);
    s0 += d * d;
  }
  return s0 + s1 + s2 + s3;
}

inline double l2_distance(const std::vector<float> &a, const std::vector<float> &b) {
  return std::sqrt(l2_distance_sq(a, b));
}

}  // namespace mvdb
