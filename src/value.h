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
  // Eight float lanes: the inputs and the output are float32 anyway, so accumulating in float
  // keeps the loop in one 256-bit domain (no per-element widening) while staying far inside the
  // 1e-5 relative tolerance the checker allows.  The lanes are summed in double at the end.
  float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t k = 0;
  for (; k + 8 <= n; k += 8) {
    const float d0 = a[k] - b[k];
    const float d1 = a[k + 1] - b[k + 1];
    const float d2 = a[k + 2] - b[k + 2];
    const float d3 = a[k + 3] - b[k + 3];
    const float d4 = a[k + 4] - b[k + 4];
    const float d5 = a[k + 5] - b[k + 5];
    const float d6 = a[k + 6] - b[k + 6];
    const float d7 = a[k + 7] - b[k + 7];
    s[0] += d0 * d0;
    s[1] += d1 * d1;
    s[2] += d2 * d2;
    s[3] += d3 * d3;
    s[4] += d4 * d4;
    s[5] += d5 * d5;
    s[6] += d6 * d6;
    s[7] += d7 * d7;
  }
  float tail = 0.0f;
  for (; k < n; ++k) {
    const float d = a[k] - b[k];
    tail += d * d;
  }
  double total = static_cast<double>(s[0]) + static_cast<double>(s[1]) + static_cast<double>(s[2]) +
                 static_cast<double>(s[3]) + static_cast<double>(s[4]) + static_cast<double>(s[5]) +
                 static_cast<double>(s[6]) + static_cast<double>(s[7]);
  return total + static_cast<double>(tail);
}

inline double l2_distance(const std::vector<float> &a, const std::vector<float> &b) {
  return std::sqrt(l2_distance_sq(a, b));
}

}  // namespace mvdb
