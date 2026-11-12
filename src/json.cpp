// Minimal JSON parser/serializer for the MiniVectorDB protocol.
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mvdb {
namespace {

struct P {
  const char *p;
  const char *end;
  bool fail = false;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
      ++p;
    }
  }
  bool lit(const char *word) {
    const size_t n = std::strlen(word);
    if (static_cast<size_t>(end - p) < n || std::memcmp(p, word, n) != 0) {
      return false;
    }
    p += n;
    return true;
  }
  bool parse(JsonValue &v, int depth);
  bool parse_string(std::string &out);
};

bool P::parse_string(std::string &out) {
  out.clear();
  if (p >= end || *p != '"') {
    return false;
  }
  ++p;
  while (p < end) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"') {
      ++p;
      return true;
    }
    if (c != '\\') {
      out.push_back(static_cast<char>(c));
      ++p;
      continue;
    }
    ++p;
    if (p >= end) {
      return false;
    }
    switch (*p) {
      case '"': out.push_back('"'); ++p; break;
      case '\\': out.push_back('\\'); ++p; break;
      case '/': out.push_back('/'); ++p; break;
      case 'b': out.push_back('\b'); ++p; break;
      case 'f': out.push_back('\f'); ++p; break;
      case 'n': out.push_back('\n'); ++p; break;
      case 'r': out.push_back('\r'); ++p; break;
      case 't': out.push_back('\t'); ++p; break;
      case 'u': {
        if (end - p < 5) {
          return false;
        }
        auto hex4 = [&](const char *q, unsigned &v4) {
          v4 = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = q[k];
            v4 <<= 4;
            if (h >= '0' && h <= '9') {
              v4 |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              v4 |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              v4 |= static_cast<unsigned>(h - 'A' + 10);
            } else {
              return false;
            }
          }
          return true;
        };
        unsigned cp = 0;
        if (!hex4(p + 1, cp)) {
          return false;
        }
        p += 5;
        // Surrogate pair
        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
          unsigned lo = 0;
          if (hex4(p + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        if (cp < 0x80) {
          out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
          out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

bool P::parse(JsonValue &v, int depth) {
  if (depth > 64) {
    return false;
  }
  ws();
  if (p >= end) {
    return false;
  }
  switch (*p) {
    case 'n':
      if (!lit("null")) return false;
      v.type = JsonValue::Type::Null;
      return true;
    case 't':
      if (!lit("true")) return false;
      v.type = JsonValue::Type::Bool;
      v.b = true;
      return true;
    case 'f':
      if (!lit("false")) return false;
      v.type = JsonValue::Type::Bool;
      v.b = false;
      return true;
    case '"':
      v.type = JsonValue::Type::String;
      return parse_string(v.str);
    case '[': {
      ++p;
      v.type = JsonValue::Type::Array;
      ws();
      if (p < end && *p == ']') {
        ++p;
        return true;
      }
      for (;;) {
        JsonValue item;
        if (!parse(item, depth + 1)) return false;
        v.arr.push_back(std::move(item));
        ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == ']') {
          ++p;
          return true;
        }
        return false;
      }
    }
    case '{': {
      ++p;
      v.type = JsonValue::Type::Object;
      ws();
      if (p < end && *p == '}') {
        ++p;
        return true;
      }
      for (;;) {
        ws();
        std::string key;
        if (!parse_string(key)) return false;
        ws();
        if (p >= end || *p != ':') return false;
        ++p;
        JsonValue item;
        if (!parse(item, depth + 1)) return false;
        v.obj.emplace_back(std::move(key), std::move(item));
        ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == '}') {
          ++p;
          return true;
        }
        return false;
      }
    }
    default: {
      // number
      const char *begin = p;
      if (*p == '-' || *p == '+') ++p;
      bool digits = false;
      while (p < end && *p >= '0' && *p <= '9') { ++p; digits = true; }
      bool fractional = false;
      if (p < end && *p == '.') {
        fractional = true;
        ++p;
        while (p < end && *p >= '0' && *p <= '9') { ++p; digits = true; }
      }
      if (p < end && (*p == 'e' || *p == 'E')) {
        fractional = true;
        ++p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
      }
      if (!digits) {
        return false;
      }
      const std::string text(begin, p - begin);
      v.type = JsonValue::Type::Number;
      v.is_int = !fractional;
      if (v.is_int) {
        errno = 0;
        char *endp = nullptr;
        const long long n = std::strtoll(text.c_str(), &endp, 10);
        if (errno == 0 && endp != text.c_str() && *endp == '\0') {
          v.i = static_cast<int64_t>(n);
        } else {
          v.is_int = false;
        }
      }
      v.num = std::strtod(text.c_str(), nullptr);
      return true;
    }
  }
}

void dump_to(const JsonValue &v, std::string &out) {
  switch (v.type) {
    case JsonValue::Type::Null:
      out += "null";
      break;
    case JsonValue::Type::Bool:
      out += v.b ? "true" : "false";
      break;
    case JsonValue::Type::Number: {
      if (v.is_int) {
        out += std::to_string(v.i);
      } else {
        char buf[64];
        // Shortest round-trippable representation, never NaN/Infinity.
        if (!std::isfinite(v.num)) {
          out += "null";
        } else {
          std::snprintf(buf, sizeof(buf), "%.17g", v.num);
          out += buf;
        }
      }
      break;
    }
    case JsonValue::Type::String:
      out += '"';
      out += json_escape(v.str);
      out += '"';
      break;
    case JsonValue::Type::Array: {
      out += '[';
      for (size_t k = 0; k < v.arr.size(); ++k) {
        if (k != 0) out += ',';
        dump_to(v.arr[k], out);
      }
      out += ']';
      break;
    }
    case JsonValue::Type::Object: {
      out += '{';
      for (size_t k = 0; k < v.obj.size(); ++k) {
        if (k != 0) out += ',';
        out += '"';
        out += json_escape(v.obj[k].first);
        out += "\":";
        dump_to(v.obj[k].second, out);
      }
      out += '}';
      break;
    }
  }
}

}  // namespace

const JsonValue *JsonValue::find(const std::string &key) const {
  for (const auto &kv : obj) {
    if (kv.first == key) {
      return &kv.second;
    }
  }
  return nullptr;
}

std::string JsonValue::dump() const {
  std::string out;
  dump_to(*this, out);
  return out;
}

bool json_parse(const std::string &text, JsonValue &out) {
  P p{text.data(), text.data() + text.size()};
  out = JsonValue();
  if (!p.parse(out, 0)) {
    return false;
  }
  p.ws();
  return p.p == p.end;
}

std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

}  // namespace mvdb
