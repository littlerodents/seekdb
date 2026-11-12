// Minimal JSON support for the stdin/stdout protocol.
//
// The protocol only needs to parse one flat request object and serialize responses, so a
// small self-contained implementation keeps the container free of dependencies.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mvdb {

struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool b = false;
  double num = 0.0;
  bool is_int = false;      // number arrived with no fraction/exponent
  int64_t i = 0;
  std::string str;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue>> obj;

  const JsonValue *find(const std::string &key) const;
  std::string dump() const;
};

// Parses exactly one JSON value from `text`; returns false on malformed input.
bool json_parse(const std::string &text, JsonValue &out);

// Escapes a string for JSON output.
std::string json_escape(const std::string &s);

}  // namespace mvdb
