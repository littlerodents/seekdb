/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <glob.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <fnmatch.h>
#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "common/timezone/ob_time_convert.h"
#include "share/ob_errno.h"
#include "lib/compress/zstd_1_3_8/ob_zstd_wrapper.h"
#include "lib/string/ob_sql_string.h"
#include <zlib.h>
#include <rapidjson/document.h>

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{
/////////////////////////////////////////////////////////////////////////////////
// small helpers
/////////////////////////////////////////////////////////////////////////////////

int copy_obstring(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(alloc.alloc(src.length()));
  if (OB_ISNULL(buf) && src.length() > 0) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("alloc failed for file sql string", K(ret), K(src.length()));
  } else {
    MEMCPY(buf, src.ptr(), src.length());
    dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
  }
  return ret;
}

bool ieq(const ObString &s, const char *t)
{
  int64_t n = static_cast<int64_t>(strlen(t));
  if (s.length() != n) {
    return false;
  }
  for (int64_t i = 0; i < n; ++i) {
    if (std::toupper(static_cast<unsigned char>(s.ptr()[i])) !=
        std::toupper(static_cast<unsigned char>(t[i]))) {
      return false;
    }
  }
  return true;
}

bool is_null_obj(const ObObj &o) { return o.is_null(); }

// strict integer parse of the WHOLE string (no trailing garbage)
bool parse_int64_strict(const ObString &s, int64_t &out)
{
  if (s.length() == 0) {
    return false;
  }
  int64_t i = 0;
  bool neg = false;
  if (s.ptr()[0] == '+' || s.ptr()[0] == '-') {
    neg = (s.ptr()[0] == '-');
    i = 1;
    if (s.length() == 1) {
      return false;
    }
  }
  if (i >= s.length()) {
    return false;
  }
  // reject leading zeros for multi-digit numbers? DuckDB keeps them as
  // integers too, so accept.
  // Parse the magnitude as unsigned: INT64_MIN has one more unit of
  // magnitude than INT64_MAX and cannot be accumulated in a signed int64.
  uint64_t v = 0;
  const uint64_t limit = neg ? static_cast<uint64_t>(INT64_MAX) + 1 : INT64_MAX;
  for (; i < s.length(); ++i) {
    char c = s.ptr()[i];
    if (c < '0' || c > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (v > (limit - digit) / 10) {
      return false; // overflow -> caller may retry as double
    }
    v = v * 10 + digit;
  }
  out = neg ? (v == limit ? INT64_MIN : -static_cast<int64_t>(v))
            : static_cast<int64_t>(v);
  return true;
}

bool is_int_overflow_double_candidate(const ObString &s)
{
  // integer-looking but beyond int64: DuckDB sniffs it as DOUBLE
  int64_t i = 0;
  if (s.length() > 0 && (s.ptr()[0] == '+' || s.ptr()[0] == '-')) {
    i = 1;
  }
  if (i >= s.length()) {
    return false;
  }
  for (; i < s.length(); ++i) {
    if (s.ptr()[i] < '0' || s.ptr()[i] > '9') {
      return false;
    }
  }
  return true;
}

// strict double parse of the WHOLE string
bool parse_double_strict(const ObString &s, double &out)
{
  if (s.length() == 0 || s.length() > 512) {
    return false;
  }
  char buf[513];
  MEMCPY(buf, s.ptr(), s.length());
  buf[s.length()] = '\0';
  char *end = nullptr;
  errno = 0;
  double v = strtod(buf, &end);
  if (end != buf + s.length() || errno == ERANGE) {
    return false;
  }
  if (std::isnan(v) || std::isinf(v)) {
    return false;
  }
  out = v;
  return true;
}

// DATE-like: ^\d{4}-\d{2}-\d{2}$
bool looks_like_date(const ObString &s)
{
  if (s.length() != 10) {
    return false;
  }
  const char *p = s.ptr();
  for (int64_t i = 0; i < 10; ++i) {
    char c = p[i];
    if (i == 4 || i == 7) {
      if (c != '-') {
        return false;
      }
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// TIME-like: ^\d{2}:\d{2}(:\d{2}(\.\d+)?)?$
bool looks_like_time(const ObString &s, int16_t &scale)
{
  scale = 0;
  if (s.length() < 5) {
    return false;
  }
  const char *p = s.ptr();
  auto two_digits = [&](int64_t at) { return p[at] >= '0' && p[at] <= '9' && p[at + 1] >= '0' && p[at + 1] <= '9'; };
  if (!two_digits(0) || p[2] != ':' || !two_digits(3)) {
    return false;
  }
  const int hour = (p[0] - '0') * 10 + (p[1] - '0');
  const int minute = (p[3] - '0') * 10 + (p[4] - '0');
  if (hour > 23 || minute > 59) {
    return false;
  }
  int64_t len = s.length();
  if (len == 5) {
    return true;
  }
  if (len < 8) {
    return false;
  }
  if (p[5] != ':') {
    return false;
  }
  if (!two_digits(6)) {
    return false;
  }
  const int second = (p[6] - '0') * 10 + (p[7] - '0');
  if (second > 59) {
    return false;
  }
  if (len == 8) {
    return true;
  }
  if (p[8] != '.') {
    return false;
  }
  int64_t frac = 0;
  for (int64_t i = 9; i < len; ++i) {
    if (p[i] < '0' || p[i] > '9') {
      return false;
    }
    ++frac;
  }
  if (frac == 0 || frac > 6) {
    return false;
  }
  scale = static_cast<int16_t>(frac);
  return true;
}

// DATETIME-like: date [T|space] time (no timezone suffix in the candidate)
bool looks_like_datetime(const ObString &s, int16_t &scale)
{
  scale = 0;
  if (s.length() < 19 || !looks_like_date(ObString(10, s.ptr()))) {
    return false;
  }
  const char *p = s.ptr();
  if (p[10] != ' ' && p[10] != 'T') {
    return false;
  }
  int16_t tscale = 0;
  ObString tpart(static_cast<int32_t>(s.length() - 11), p + 11);
  if (!looks_like_time(tpart, tscale)) {
    return false;
  }
  // full form requires seconds
  if (tpart.length() == 5) {
    return false;
  }
  scale = tscale;
  return true;
}

/////////////////////////////////////////////////////////////////////////////////
// parquet thrift compact protocol reader (subset)
/////////////////////////////////////////////////////////////////////////////////

class ThriftReader
{
public:
  ThriftReader(const char *buf, int64_t len) : buf_(buf), len_(len), pos_(0) {}
  virtual ~ThriftReader() {}
  inline bool has(int64_t n) const { return n >= 0 && pos_ >= 0 && pos_ <= len_ && n <= len_ - pos_; }
  inline int64_t pos() const { return pos_; }
  inline void set_pos(int64_t p) { pos_ = p; }

  int read_byte(int8_t &v)
  {
    int ret = OB_SUCCESS;
    if (!has(1)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      v = buf_[pos_++];
    }
    return ret;
  }

  int read_varint_u64(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    while (OB_SUCC(ret)) {
      if (!has(1) || shift > 63) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        uint8_t b = static_cast<uint8_t>(buf_[pos_++]);
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) {
          break;
        }
        shift += 7;
      }
    }
    return ret;
  }

  int read_zigzag_i64(int64_t &v)
  {
    int ret = OB_SUCCESS;
    uint64_t u = 0;
    if (OB_FAIL(read_varint_u64(u))) {
    } else {
      v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    }
    return ret;
  }

  int read_i32(int32_t &v)
  {
    int ret = OB_SUCCESS;
    int64_t t = 0;
    if (OB_FAIL(read_zigzag_i64(t)) || t < INT32_MIN || t > INT32_MAX) {
      ret = OB_SUCCESS == ret ? OB_INVALID_ARGUMENT : ret;
    } else {
      v = static_cast<int32_t>(t);
    }
    return ret;
  }

  int read_i64(int64_t &v) { return read_zigzag_i64(v); }

  int read_string(ObIAllocator &alloc, ObString &v)
  {
    int ret = OB_SUCCESS;
    uint64_t raw_len = 0;
    if (OB_FAIL(read_varint_u64(raw_len)) || raw_len > INT64_MAX || !has(static_cast<int64_t>(raw_len))) {
      if (OB_SUCC(ret)) {
        ret = OB_INVALID_ARGUMENT;
      }
    } else {
      const int64_t len = static_cast<int64_t>(raw_len);
      char *buf = static_cast<char *>(alloc.alloc(len));
      if (OB_ISNULL(buf) && len > 0) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, buf_ + pos_, len);
        v.assign_ptr(buf, static_cast<int32_t>(len));
        pos_ += len;
      }
    }
    return ret;
  }

  // field header: returns false at struct STOP
  int read_field_begin(int8_t &type, int16_t &fid, bool &stop, int16_t &last_fid)
  {
    int ret = OB_SUCCESS;
    stop = false;
    int8_t b = 0;
    if (OB_FAIL(read_byte(b))) {
    } else if (b == 0) {
      stop = true;
    } else {
      int8_t delta = static_cast<int8_t>((b >> 4) & 0x0F);
      type = static_cast<int8_t>(b & 0x0F);
      if (delta != 0) {
        fid = static_cast<int16_t>(last_fid + delta);
      } else {
        int16_t explicit_id = 0;
        if (OB_FAIL(read_zigzag_i64_as_i16(explicit_id))) {
        } else {
          fid = explicit_id;
        }
      }
      if (OB_SUCC(ret)) {
        last_fid = fid;
      }
    }
    return ret;
  }

  int read_zigzag_i64_as_i16(int16_t &v)
  {
    int ret = OB_SUCCESS;
    int64_t t = 0;
    if (OB_FAIL(read_zigzag_i64(t)) || t < INT16_MIN || t > INT16_MAX) {
      if (OB_SUCC(ret)) {
        ret = OB_INVALID_ARGUMENT;
      }
    } else {
      v = static_cast<int16_t>(t);
    }
    return ret;
  }

  // list header: elem type + size
  int read_list_begin(int8_t &elem_type, int64_t &size)
  {
    int ret = OB_SUCCESS;
    int8_t b = 0;
    if (OB_FAIL(read_byte(b))) {
    } else {
      elem_type = static_cast<int8_t>(b & 0x0F);
      int8_t sz = static_cast<int8_t>((b >> 4) & 0x0F);
      if (sz == 15) {
        uint64_t t = 0;
        if (OB_FAIL(read_varint_u64(t)) || t > INT64_MAX) {
          if (OB_SUCC(ret)) {
            ret = OB_INVALID_ARGUMENT;
          }
        } else {
          size = static_cast<int64_t>(t);
        }
      } else {
        size = sz;
      }
    }
    return ret;
  }

  // skip a value of the given compact type (recursively for containers)
  int skip_value(int8_t type)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case 1: // bool true
      case 2: { // bool false
        break;
      }
      case 3: { // byte
        if (!has(1)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          pos_ += 1;
        }
        break;
      }
      case 4:  // i16
      case 5:  // i32
      case 6: { // i64
        int64_t v = 0;
        ret = read_zigzag_i64(v);
        break;
      }
      case 7: { // double
        if (!has(8)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          pos_ += 8;
        }
        break;
      }
      case 8: { // binary
        uint64_t len = 0;
        if (OB_FAIL(read_varint_u64(len)) || len > INT64_MAX || !has(static_cast<int64_t>(len))) {
          if (OB_SUCC(ret)) {
            ret = OB_INVALID_ARGUMENT;
          }
        } else {
          pos_ += static_cast<int64_t>(len);
        }
        break;
      }
      case 9:  // list
      case 10: { // set
        int8_t elem_type = 0;
        int64_t size = 0;
        if (OB_FAIL(read_list_begin(elem_type, size))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          if (1 == elem_type || 2 == elem_type) {
            int8_t ignored = 0;
            ret = read_byte(ignored);
          } else {
            ret = skip_value(elem_type);
          }
        }
        break;
      }
      case 11: { // map
        uint64_t size = 0;
        if (OB_FAIL(read_varint_u64(size))) {
        } else if (size > static_cast<uint64_t>(len_ - pos_)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (size != 0) {
          int8_t kv = 0;
          if (OB_FAIL(read_byte(kv))) {
          }
          int8_t key_type = static_cast<int8_t>((kv >> 4) & 0x0F);
          int8_t val_type = static_cast<int8_t>(kv & 0x0F);
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            if (OB_FAIL(skip_value(key_type))) {
            } else {
              ret = skip_value(val_type);
            }
          }
        }
        break;
      }
      case 12: { // struct
        int16_t nested_last = 0;
        int16_t nested_fid = 0;
        int8_t nested_type = 0;
        bool nested_stop = false;
        while (OB_SUCC(ret) && !nested_stop) {
          if (OB_FAIL(read_field_begin(nested_type, nested_fid, nested_stop, nested_last))) {
          } else if (!nested_stop) {
            ret = skip_value(nested_type);
          }
        }
        break;
      }
      default: {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
    }
    return ret;
  }

private:
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

/////////////////////////////////////////////////////////////////////////////////
// parquet metadata structures
/////////////////////////////////////////////////////////////////////////////////

struct ParquetSchemaElement
{
  ParquetSchemaElement()
    : type_(-1), type_length_(0), repetition_type_(-1), num_children_(0),
      converted_type_(-1), scale_(0), precision_(0), has_logical_(false),
      logical_string_(false), logical_date_(false), logical_time_unit_(0),
      logical_time_has_unit_(false), logical_ts_unit_(0), logical_ts_utc_(false), logical_int_bits_(0),
      logical_int_signed_(true), logical_decimal_(false) {}
  int32_t type_;            // physical type, -1 for groups
  int32_t type_length_;     // FLBA length
  int32_t repetition_type_; // 0 required / 1 optional / 2 repeated
  ObString name_;
  int32_t num_children_;
  int32_t converted_type_;  // -1 none
  int32_t scale_;
  int32_t precision_;
  bool has_logical_;
  bool logical_string_;
  bool logical_date_;
  int32_t logical_time_unit_; // 1 millis 2 micros 3 nanos
  bool logical_time_has_unit_;
  int32_t logical_ts_unit_;
  bool logical_ts_utc_;
  int32_t logical_int_bits_;
  bool logical_int_signed_;
  bool logical_decimal_;
};

struct ParquetColumnChunkMeta
{
  ParquetColumnChunkMeta()
    : codec_(0), num_values_(0), data_page_offset_(-1),
      dictionary_page_offset_(-1) {}
  ObString column_name_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
};

// parse a LogicalType struct starting after its field header sequence was consumed
static int parse_parquet_logical_type_precise(ThriftReader &r, ObIAllocator &alloc,
                                              ParquetSchemaElement &e);

static int parse_parquet_schema_element(ThriftReader &r, ObIAllocator &alloc,
                                        ParquetSchemaElement &e)
{
  int ret = OB_SUCCESS;
  int16_t fid = 0;
  int16_t last_fid = 0;
  int8_t ftype = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(r.read_field_begin(ftype, fid, stop, last_fid))) {
    } else if (stop) {
      break;
    } else {
      switch (fid) {
        case 1: {
          ret = r.read_i32(e.type_);
          break;
        }
        case 2: {
          ret = r.read_i32(e.type_length_);
          break;
        }
        case 3: {
          ret = r.read_i32(e.repetition_type_);
          break;
        }
        case 4: {
          ret = r.read_string(alloc, e.name_);
          break;
        }
        case 5: {
          ret = r.read_i32(e.num_children_);
          break;
        }
        case 6: {
          ret = r.read_i32(e.converted_type_);
          break;
        }
        case 7: {
          ret = r.read_i32(e.scale_);
          break;
        }
        case 8: {
          ret = r.read_i32(e.precision_);
          break;
        }
        case 10: {
          // LogicalType struct
          if (12 != ftype) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            e.has_logical_ = true;
            ret = parse_parquet_logical_type_precise(r, alloc, e);
          }
          break;
        }
        default: {
          ret = r.skip_value(ftype);
          break;
        }
      }
    }
  }
  return ret;
}

// The generic logical-type walker above is intentionally shallow; for the
// time/timestamp units we need precise values, so re-implement focused parse.
static int parse_parquet_logical_type_precise(ThriftReader &r, ObIAllocator &alloc,
                                              ParquetSchemaElement &e)
{
  int ret = OB_SUCCESS;
  int16_t last_fid = 0;
  while (OB_SUCC(ret)) {
    int16_t fid = 0;
    int8_t ftype = 0;
    bool stop = false;
    if (OB_FAIL(r.read_field_begin(ftype, fid, stop, last_fid))) {
    } else if (stop) {
      break;
    } else if (fid == 7 && 12 == ftype) {
      // TimeType {1: isAdjustedToUTC, 2: TimeUnit}
      int16_t last = 0;
      while (OB_SUCC(ret)) {
        int16_t tfid = 0;
        int8_t ttype = 0;
        bool tstop = false;
        if (OB_FAIL(r.read_field_begin(ttype, tfid, tstop, last))) {
        } else if (tstop) {
          break;
        } else if (tfid == 2 && 12 == ttype) {
          int16_t ulast = 0;
          while (OB_SUCC(ret)) {
            int16_t ufid = 0;
            int8_t utype = 0;
            bool ustop = false;
            if (OB_FAIL(r.read_field_begin(utype, ufid, ustop, ulast))) {
            } else if (ustop) {
              break;
            } else if (ufid >= 1 && ufid <= 3) {
              // 1 MILLIS 2 MICROS 3 NANOS, each an empty struct
              e.logical_time_has_unit_ = true;
              e.logical_time_unit_ = ufid;
              ret = r.skip_value(utype);
            } else {
              ret = r.skip_value(utype);
            }
          }
        } else {
          ret = r.skip_value(ttype);
        }
      }
    } else if (fid == 8 && 12 == ftype) {
      // TimestampType {1: isAdjustedToUTC, 2: TimeUnit}
      int16_t last = 0;
      while (OB_SUCC(ret)) {
        int16_t tfid = 0;
        int8_t ttype = 0;
        bool tstop = false;
        if (OB_FAIL(r.read_field_begin(ttype, tfid, tstop, last))) {
        } else if (tstop) {
          break;
        } else if (tfid == 1 && (1 == ttype || 2 == ttype)) {
          e.logical_ts_utc_ = (1 == ttype);
        } else if (tfid == 2 && 12 == ttype) {
          int16_t ulast = 0;
          while (OB_SUCC(ret)) {
            int16_t ufid = 0;
            int8_t utype = 0;
            bool ustop = false;
            if (OB_FAIL(r.read_field_begin(utype, ufid, ustop, ulast))) {
            } else if (ustop) {
              break;
            } else if (ufid >= 1 && ufid <= 3) {
              e.logical_ts_unit_ = ufid;
              ret = r.skip_value(utype);
            } else {
              ret = r.skip_value(utype);
            }
          }
        } else {
          ret = r.skip_value(ttype);
        }
      }
    } else if (fid == 5 && 12 == ftype) {
      // DecimalType {1: scale, 2: precision}
      int16_t last = 0;
      while (OB_SUCC(ret)) {
        int16_t tfid = 0;
        int8_t ttype = 0;
        bool tstop = false;
        if (OB_FAIL(r.read_field_begin(ttype, tfid, tstop, last))) {
        } else if (tstop) {
          break;
        } else if (tfid == 1 && (5 == ttype || 6 == ttype)) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag_i64(v))) {
          } else {
            e.scale_ = static_cast<int32_t>(v);
            e.logical_decimal_ = true;
          }
        } else if (tfid == 2 && (5 == ttype || 6 == ttype)) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag_i64(v))) {
          } else {
            e.precision_ = static_cast<int32_t>(v);
            e.logical_decimal_ = true;
          }
        } else {
          ret = r.skip_value(ttype);
        }
      }
    } else if (fid == 10 && 12 == ftype) {
      // IntType {1: bitWidth, 2: isSigned} - values arrive as i32/bool
      int16_t last = 0;
      while (OB_SUCC(ret)) {
        int16_t tfid = 0;
        int8_t ttype = 0;
        bool tstop = false;
        if (OB_FAIL(r.read_field_begin(ttype, tfid, tstop, last))) {
        } else if (tstop) {
          break;
        } else if (tfid == 1) {
          int32_t bits = 0;
          if (OB_FAIL(r.read_i32(bits))) {
          } else {
            e.logical_int_bits_ = bits;
          }
        } else if (tfid == 2 && (1 == ttype || 2 == ttype)) {
          e.logical_int_signed_ = (1 == ttype);
        } else {
          ret = r.skip_value(ttype);
        }
      }
    } else if (fid == 1 && 12 == ftype) {
      e.logical_string_ = true;
      ret = r.skip_value(ftype);
    } else if (fid == 6 && 12 == ftype) {
      e.logical_date_ = true;
      ret = r.skip_value(ftype);
    } else {
      ret = r.skip_value(ftype);
    }
  }
  return ret;
}

static int parse_parquet_column_meta(ThriftReader &r, ObIAllocator &alloc,
                                     ParquetColumnChunkMeta &m)
{
  int ret = OB_SUCCESS;
  int16_t fid = 0;
  int16_t last_fid = 0;
  int8_t ftype = 0;
  bool stop = false;
  bool in_nested = false;
  int16_t outer_last_fid = 0;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(r.read_field_begin(ftype, fid, stop, last_fid))) {
    } else if (stop) {
      if (in_nested) {
        in_nested = false;
        stop = false;
        last_fid = outer_last_fid;
      } else {
        break;
      }
    } else if (in_nested) {
      // inside ColumnMetaData struct
      switch (fid) {
        case 1: { // type
          ret = r.skip_value(ftype);
          break;
        }
        case 2: { // encodings list
          ret = r.skip_value(ftype);
          break;
        }
        case 3: { // path_in_schema
          if (9 != ftype) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            int8_t elem_type = 0;
            int64_t size = 0;
            if (OB_FAIL(r.read_list_begin(elem_type, size)) || size < 1) {
              if (OB_SUCC(ret)) {
                ret = OB_INVALID_ARGUMENT;
              }
            } else {
              for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
                if (i == 0) {
                  ret = r.read_string(alloc, m.column_name_);
                } else {
                  ObString tmp;
                  ret = r.read_string(alloc, tmp);
                }
              }
            }
          }
          break;
        }
        case 4: { // codec
          ret = r.read_i32(m.codec_);
          break;
        }
        case 5: { // num_values
          ret = r.read_i64(m.num_values_);
          break;
        }
        case 6:
        case 7: { // sizes
          ret = r.skip_value(ftype);
          break;
        }
        case 9: { // data_page_offset
          ret = r.read_i64(m.data_page_offset_);
          break;
        }
        case 10: { // index_page_offset
          ret = r.skip_value(ftype);
          break;
        }
        case 11: { // dictionary_page_offset
          ret = r.read_i64(m.dictionary_page_offset_);
          break;
        }
        default: {
          ret = r.skip_value(ftype);
          break;
        }
      }
    } else {
      if (fid == 3 && 12 == ftype) {
        in_nested = true;
        outer_last_fid = last_fid;
        last_fid = 0;
      } else {
        ret = r.skip_value(ftype);
      }
    }
  }
  return ret;
}

/////////////////////////////////////////////////////////////////////////////////
// decompressors (raw block formats)
/////////////////////////////////////////////////////////////////////////////////

int snappy_uncompress(const char *src, int64_t src_len, char *dst, int64_t dst_cap,
                      int64_t &out_len)
{
  int ret = OB_SUCCESS;
  int64_t ip = 0;
  if (src_len < 1) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // varint uncompressed length
    uint64_t ulen = 0;
    int shift = 0;
    while (true) {
      if (ip >= src_len || shift > 63) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
      uint8_t b = static_cast<uint8_t>(src[ip++]);
      ulen |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) {
        break;
      }
      shift += 7;
    }
    if (OB_FAIL(ret)) {
    } else if (static_cast<int64_t>(ulen) > dst_cap) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      int64_t op = 0;
      while (OB_SUCC(ret) && ip < src_len) {
        uint8_t tag = static_cast<uint8_t>(src[ip++]);
        uint32_t len = 0, off = 0;
        if ((tag & 0x03) == 0) { // literal
          len = (tag >> 2) + 1;
          if (len > 60) {
            int extra = len - 60;
            len = 0;
            for (int i = 0; i < extra; ++i) {
              if (ip >= src_len) {
                ret = OB_INVALID_ARGUMENT;
                break;
              }
              len |= static_cast<uint32_t>(static_cast<uint8_t>(src[ip++])) << (8 * i);
            }
            len += 1;
          }
          if (ip + static_cast<int64_t>(len) > src_len || op + static_cast<int64_t>(len) > dst_cap) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            MEMCPY(dst + op, src + ip, len);
            ip += len;
            op += len;
          }
        } else if ((tag & 0x03) == 1) { // copy 1-byte offset
          if (ip >= src_len) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            uint8_t b = static_cast<uint8_t>(src[ip++]);
            len = ((tag >> 2) & 0x07) + 4;
            // The top three tag bits are the high bits of the 11-bit offset.
            off = ((static_cast<uint32_t>(tag) & 0xe0U) << 3) | static_cast<uint32_t>(b);
          }
        } else if ((tag & 0x03) == 2) { // copy 2-byte offset
          if (ip + 2 > src_len) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            len = (tag >> 2) + 1;
            off = static_cast<uint32_t>(static_cast<uint8_t>(src[ip])) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(src[ip + 1])) << 8);
            ip += 2;
          }
        } else { // copy 4-byte offset
          if (ip + 4 > src_len) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            len = (tag >> 2) + 1;
            off = static_cast<uint32_t>(static_cast<uint8_t>(src[ip])) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(src[ip + 1])) << 8) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(src[ip + 2])) << 16) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(src[ip + 3])) << 24);
            ip += 4;
          }
        }
        if (OB_SUCC(ret) && (tag & 0x03) != 0) {
          if (off == 0 || off > static_cast<uint32_t>(op) ||
              op + static_cast<int64_t>(len) > dst_cap) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            for (uint32_t i = 0; i < len; ++i) {
              dst[op + i] = dst[op - off + i];
            }
            op += len;
          }
        }
      }
      if (OB_SUCC(ret) && op != static_cast<int64_t>(ulen)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        out_len = op;
      }
    }
  }
  return ret;
}

int lz4_uncompress_block(const char *src, int64_t src_len, char *dst, int64_t dst_cap,
                         int64_t &out_len)
{
  int ret = OB_SUCCESS;
  int64_t ip = 0, op = 0;
  while (OB_SUCC(ret) && ip < src_len) {
    uint8_t token = static_cast<uint8_t>(src[ip++]);
    uint32_t lit_len = token >> 4;
    if (lit_len == 15) {
      while (true) {
        if (ip >= src_len) {
          ret = OB_INVALID_ARGUMENT;
          break;
        }
        uint8_t b = static_cast<uint8_t>(src[ip++]);
        lit_len += b;
        if (b != 255) {
          break;
        }
      }
    }
    if (ip + static_cast<int64_t>(lit_len) > src_len ||
        op + static_cast<int64_t>(lit_len) > dst_cap) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(dst + op, src + ip, lit_len);
      ip += lit_len;
      op += lit_len;
      if (ip >= src_len) {
        break; // last literals
      }
      if (ip + 2 > src_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        uint32_t match_len = (token & 0x0F) + 4;
        uint32_t off = static_cast<uint32_t>(static_cast<uint8_t>(src[ip])) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(src[ip + 1])) << 8);
        ip += 2;
        if (match_len - 4 == 15) {
          while (true) {
            if (ip >= src_len) {
              ret = OB_INVALID_ARGUMENT;
              break;
            }
            uint8_t b = static_cast<uint8_t>(src[ip++]);
            match_len += b;
            if (b != 255) {
              break;
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (off == 0 || off > static_cast<uint32_t>(op) ||
                   op + static_cast<int64_t>(match_len) > dst_cap) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          for (uint32_t i = 0; i < match_len; ++i) {
            dst[op + i] = dst[op - off + i];
          }
          op += match_len;
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    out_len = op;
  }
  return ret;
}

int zlib_family_inflate(const char *src, int64_t src_len, char *dst, int64_t dst_cap,
                        int64_t &out_len, bool maybe_gzip)
{
  int ret = OB_SUCCESS;
  z_stream strm;
  MEMSET(&strm, 0, sizeof(strm));
  // 15+16=gzip, 15=zlib, 15+32=auto gzip/zlib
  int window = maybe_gzip ? (15 + 32) : 15;
  if (Z_OK != inflateInit2(&strm, window)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src));
    strm.avail_in = static_cast<uInt>(src_len);
    strm.next_out = reinterpret_cast<Bytef *>(dst);
    strm.avail_out = static_cast<uInt>(dst_cap);
    int zr = inflate(&strm, Z_FINISH);
    if ((zr != Z_STREAM_END && zr != Z_OK) || strm.avail_out == 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      out_len = dst_cap - strm.avail_out;
    }
    inflateEnd(&strm);
  }
  return ret;
}

int parquet_decompress(int32_t codec, const char *src, int64_t src_len,
                       char *dst, int64_t dst_cap, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  switch (codec) {
    case 0: { // UNCOMPRESSED
      if (src_len > dst_cap) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        MEMCPY(dst, src, src_len);
        out_len = src_len;
      }
      break;
    }
    case 1: { // SNAPPY
      ret = snappy_uncompress(src, src_len, dst, dst_cap, out_len);
      break;
    }
    case 2: { // GZIP
      ret = zlib_family_inflate(src, src_len, dst, dst_cap, out_len, true);
      break;
    }
    case 5: { // LZ4 (hadoop-framed lz4 behaves like raw block in practice for
              // most writers; fall back to raw block)
      ret = lz4_uncompress_block(src, src_len, dst, dst_cap, out_len);
      break;
    }
    case 7: { // LZ4_RAW
      ret = lz4_uncompress_block(src, src_len, dst, dst_cap, out_len);
      break;
    }
    case 6: { // ZSTD
      char *cmem = static_cast<char *>(malloc(16));
      if (OB_ISNULL(cmem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        zstd_1_3_8::OB_ZSTD_customMem zmem;
        zmem.customAlloc = nullptr;
        zmem.customFree = nullptr;
        zmem.opaque = nullptr;
        size_t outp = 0;
        // allocate scratch and call simple decompress
        free(cmem);
        size_t zr = 0;
        // use the one-shot API through the wrapper
        // wrapper needs customMem with real allocator; use malloc-backed
        struct MallocCtx
        {
          static void *alloc_fn(void *opaque, size_t size)
          {
            UNUSED(opaque);
            return malloc(size);
          }
          static void free_fn(void *opaque, void *address)
          {
            UNUSED(opaque);
            free(address);
          }
        };
        zstd_1_3_8::OB_ZSTD_customMem zmem2;
        zmem2.customAlloc = MallocCtx::alloc_fn;
        zmem2.customFree = MallocCtx::free_fn;
        zmem2.opaque = nullptr;
        if (0 != zstd_1_3_8::ObZstdWrapper::decompress(
              zmem2, src, static_cast<size_t>(src_len), dst,
              static_cast<size_t>(dst_cap), zr)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          out_len = static_cast<int64_t>(zr);
        }
      }
      break;
    }
    default: {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet codec", K(codec));
      break;
    }
  }
  return ret;
}

/////////////////////////////////////////////////////////////////////////////////
// RLE / bit-packed hybrid decoder
/////////////////////////////////////////////////////////////////////////////////

class RleDecoder
{
public:
  RleDecoder(const char *buf, int64_t len, int32_t bit_width)
    : buf_(buf), len_(len), pos_(0), bit_width_(bit_width),
      rle_count_(0), bitpack_left_(0), bitpack_pos_(0), current_(0),
      byte_cache_(0), bit_pos_(0)
  {
    value_bytes_ = (bit_width_ + 7) / 8;
  }

  // returns OB_ITER_END when exhausted
  int next(int64_t &v)
  {
    int ret = OB_SUCCESS;
    if (rle_count_ > 0) {
      --rle_count_;
      v = current_;
    } else if (bitpack_left_ > 0) {
      ret = read_bits(v);
      --bitpack_left_;
    } else {
      ret = next_run();
      if (OB_ITER_END == ret) {
      } else if (OB_FAIL(ret)) {
      } else {
        ret = next(v);
      }
    }
    return ret;
  }

private:
  int read_uvarint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    while (true) {
      if (pos_ >= len_ || shift > 63) {
        ret = OB_ITER_END;
        break;
      }
      uint8_t b = static_cast<uint8_t>(buf_[pos_++]);
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) {
        break;
      }
      shift += 7;
    }
    return ret;
  }

  int next_run()
  {
    int ret = OB_SUCCESS;
    uint64_t header = 0;
    if (OB_FAIL(read_uvarint(header))) {
      if (OB_ITER_END != ret) {
        LOG_WARN("rle decode failed", K(ret));
      }
    } else if (header & 1) { // bit-packed run
      int64_t groups = static_cast<int64_t>(header >> 1);
      bitpack_left_ = groups * 8;
      bitpack_pos_ = 0;
      byte_cache_ = 0;
      bit_pos_ = 0;
      if (bitpack_left_ == 0) {
        ret = next_run();
      }
    } else { // rle run
      rle_count_ = static_cast<int64_t>(header >> 1);
      current_ = 0;
      if (pos_ + value_bytes_ > len_) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        for (int i = 0; i < value_bytes_; ++i) {
          current_ |= static_cast<int64_t>(static_cast<uint8_t>(buf_[pos_ + i])) << (8 * i);
        }
        pos_ += value_bytes_;
        if (rle_count_ == 0) {
          ret = next_run();
        }
      }
    }
    return ret;
  }

  int read_bits(int64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    for (int32_t i = 0; OB_SUCC(ret) && i < bit_width_; ++i) {
      if (bit_pos_ == 0) {
        if (pos_ >= len_) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          byte_cache_ = static_cast<uint8_t>(buf_[pos_++]);
          bit_pos_ = 8;
        }
      }
      if (OB_SUCC(ret)) {
        v |= static_cast<int64_t>((byte_cache_ >> (8 - bit_pos_)) & 1) << i;
        --bit_pos_;
      }
    }
    return ret;
  }

  const char *buf_;
  int64_t len_;
  int64_t pos_;
  int32_t bit_width_;
  int32_t value_bytes_;
  int64_t rle_count_;
  int64_t bitpack_left_;
  int64_t bitpack_pos_;
  int64_t current_;
  uint8_t byte_cache_;
  int32_t bit_pos_;
};

/////////////////////////////////////////////////////////////////////////////////
// parquet page decoding
/////////////////////////////////////////////////////////////////////////////////

struct ParquetPageHeader
{
  ParquetPageHeader() : type_(-1), uncompressed_size_(0), compressed_size_(0),
                        num_values_(0), encoding_(-1), def_encoding_(-1),
                        v2_num_nulls_(0), v2_def_len_(0), v2_rep_len_(0),
                        v2_is_compressed_(true) {}
  int32_t type_; // 0 data 2 dict 3 data_v2
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int64_t num_values_;
  int32_t encoding_;
  int32_t def_encoding_;
  int64_t v2_num_nulls_;
  int64_t v2_def_len_;
  int64_t v2_rep_len_;
  bool v2_is_compressed_;
};

static int parse_parquet_page_header(ThriftReader &r, ParquetPageHeader &h)
{
  int ret = OB_SUCCESS;
  int16_t fid = 0;
  int16_t last_fid = 0;
  int8_t ftype = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(r.read_field_begin(ftype, fid, stop, last_fid))) {
    } else if (stop) {
      break;
    } else {
      switch (fid) {
        case 1: {
          ret = r.read_i32(h.type_);
          break;
        }
        case 2: {
          ret = r.read_i32(h.uncompressed_size_);
          break;
        }
        case 3: {
          ret = r.read_i32(h.compressed_size_);
          break;
        }
        case 4: {
          ret = r.skip_value(ftype); // crc
          break;
        }
        case 5: { // data_page_header
          int16_t fid2 = 0, last2 = 0;
          int8_t ft2 = 0;
          bool stop2 = false;
          while (OB_SUCC(ret) && !stop2) {
            if (OB_FAIL(r.read_field_begin(ft2, fid2, stop2, last2))) {
            } else if (stop2) {
              break;
            } else if (fid2 == 1) {
              int64_t v = 0;
              if (OB_FAIL(r.read_zigzag_i64(v))) {
              } else if (v < 0) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                h.num_values_ = v;
              }
            } else if (fid2 == 2) {
              ret = r.read_i32(h.encoding_);
            } else if (fid2 == 3) {
              ret = r.read_i32(h.def_encoding_);
            } else if (fid2 == 4) {
              ret = r.skip_value(ft2); // rep level encoding
            } else {
              ret = r.skip_value(ft2);
            }
          }
          break;
        }
        case 7: { // dictionary_page_header
          int16_t fid2 = 0, last2 = 0;
          int8_t ft2 = 0;
          bool stop2 = false;
          h.type_ = 2; // normalize to dictionary page
          while (OB_SUCC(ret) && !stop2) {
            if (OB_FAIL(r.read_field_begin(ft2, fid2, stop2, last2))) {
            } else if (stop2) {
              break;
            } else if (fid2 == 1) {
              int64_t v = 0;
              if (OB_FAIL(r.read_zigzag_i64(v))) {
              } else {
                h.num_values_ = v;
              }
            } else {
              ret = r.skip_value(ft2);
            }
          }
          break;
        }
        case 8: { // data_page_header_v2
          int16_t fid2 = 0, last2 = 0;
          int8_t ft2 = 0;
          bool stop2 = false;
          h.type_ = 3;
          while (OB_SUCC(ret) && !stop2) {
            if (OB_FAIL(r.read_field_begin(ft2, fid2, stop2, last2))) {
            } else if (stop2) {
              break;
            } else {
              switch (fid2) {
                case 1: {
                  int64_t v = 0;
                  if (OB_FAIL(r.read_zigzag_i64(v))) {
                  } else {
                    h.num_values_ = v;
                  }
                  break;
                }
                case 2: {
                  int64_t v = 0;
                  if (OB_FAIL(r.read_zigzag_i64(v))) {
                  } else {
                    h.v2_num_nulls_ = v;
                  }
                  break;
                }
                case 4: {
                  ret = r.read_i32(h.encoding_);
                  break;
                }
                case 5: {
                  int64_t v = 0;
                  if (OB_FAIL(r.read_zigzag_i64(v))) {
                  } else {
                    h.v2_def_len_ = v;
                  }
                  break;
                }
                case 6: {
                  int64_t v = 0;
                  if (OB_FAIL(r.read_zigzag_i64(v))) {
                  } else {
                    h.v2_rep_len_ = v;
                  }
                  break;
                }
                case 7: {
                  if (1 == ft2) {
                    h.v2_is_compressed_ = true;
                  } else if (2 == ft2) {
                    h.v2_is_compressed_ = false;
                  }
                  break;
                }
                default: {
                  ret = r.skip_value(ft2);
                  break;
                }
              }
            }
          }
          break;
        }
        default: {
          ret = r.skip_value(ftype);
          break;
        }
      }
    }
  }
  return ret;
}

// dictionary of decoded plain values for one column chunk
struct ParquetDict
{
  ParquetDict() : valid_(false), bit_width_(0) {}
  bool valid_;
  int32_t bit_width_;
  ObSEArray<ObObj, 64> values_;
};

static int decode_plain_values(ObIAllocator &alloc, const char *data, int64_t len,
                               const ParquetSchemaElement &schema,
                               int32_t physical_type, int64_t count,
                               ObSEArray<ObObj, 64> &out)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  auto rd_i32 = [&](int32_t &v) -> bool {
    if (pos + 4 > len) { return false; }
    v = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint8_t>(data[pos])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 3])) << 24));
    pos += 4;
    return true;
  };
  auto rd_i64 = [&](int64_t &v) -> bool {
    if (pos + 8 > len) { return false; }
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
      u |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
    }
    v = static_cast<int64_t>(u);
    pos += 8;
    return true;
  };
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObObj cell;
    switch (physical_type) {
      case 0: { // BOOLEAN bit-packed
        int64_t bit_idx = i;
        int64_t byte_idx = bit_idx / 8;
        if (byte_idx >= len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          cell.set_tinyint(((static_cast<uint8_t>(data[byte_idx]) >> (bit_idx % 8)) & 1) ? 1 : 0);
        }
        break;
      }
      case 1: { // INT32
        int32_t v = 0;
        if (!rd_i32(v)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (6 == schema.converted_type_ || schema.logical_date_) {
          cell.set_date(v); // days since epoch
        } else if (7 == schema.converted_type_) { // TIME_MILLIS
          cell.set_time(static_cast<int64_t>(v) * 1000);
        } else if (5 == schema.converted_type_ || schema.logical_decimal_) {
          double d = schema.scale_ > 0 ? v / std::pow(10.0, schema.scale_) : v * 1.0;
          cell.set_double(d);
        } else {
          cell.set_int(v);
        }
        break;
      }
      case 2: { // INT64
        int64_t v = 0;
        if (!rd_i64(v)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (9 == schema.converted_type_) { // TIMESTAMP_MILLIS
          cell.set_datetime(v * 1000);
        } else if (10 == schema.converted_type_) { // TIMESTAMP_MICROS
          cell.set_datetime(v);
        } else if (schema.logical_ts_unit_ == 1) {
          cell.set_datetime(v * 1000);
        } else if (schema.logical_ts_unit_ == 2) {
          cell.set_datetime(v);
        } else if (schema.logical_ts_unit_ == 3) {
          cell.set_datetime(v / 1000);
        } else if (schema.logical_time_has_unit_ && schema.logical_time_unit_ == 2) {
          cell.set_time(v);
        } else if (schema.logical_time_has_unit_ && schema.logical_time_unit_ == 3) {
          cell.set_time(v / 1000);
        } else if (5 == schema.converted_type_ || schema.logical_decimal_) {
          double d = schema.scale_ > 0 ? v / std::pow(10.0, schema.scale_) : v * 1.0;
          cell.set_double(d);
        } else {
          cell.set_int(v);
        }
        break;
      }
      case 3: { // INT96 timestamp
        if (pos + 12 > len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          uint64_t nanos = 0;
          for (int k = 0; k < 8; ++k) {
            nanos |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + k])) << (8 * k);
          }
          int32_t julian = 0;
          for (int k = 0; k < 4; ++k) {
            julian |= static_cast<int32_t>(static_cast<uint8_t>(data[pos + 8 + k])) << (8 * k);
          }
          pos += 12;
          int64_t days = static_cast<int64_t>(julian) - 2440588;
          int64_t micros = days * 86400000000LL + static_cast<int64_t>(nanos / 1000);
          cell.set_datetime(micros);
        }
        break;
      }
      case 4: { // FLOAT
        if (pos + 4 > len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          uint32_t u = 0;
          for (int k = 0; k < 4; ++k) {
            u |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + k])) << (8 * k);
          }
          pos += 4;
          float f;
          MEMCPY(&f, &u, 4);
          cell.set_double(static_cast<double>(f));
        }
        break;
      }
      case 5: { // DOUBLE
        uint64_t u = 0;
        if (!rd_i64(reinterpret_cast<int64_t &>(u))) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          double d;
          MEMCPY(&d, &u, 8);
          cell.set_double(d);
        }
        break;
      }
      case 6: { // BYTE_ARRAY
        int32_t blen = 0;
        if (!rd_i32(blen) || blen < 0 || pos + blen > len) {
          ret = OB_INVALID_ARGUMENT;
        } else if (5 == schema.converted_type_ || schema.logical_decimal_) {
          // big-endian two's complement decimal
          int64_t v = 0;
          bool neg = (blen > 0) && (static_cast<uint8_t>(data[pos]) & 0x80);
          for (int64_t k = 0; k < blen; ++k) {
            v = (v << 8) | static_cast<uint8_t>(data[pos + k]);
          }
          if (neg && blen < 8) {
            v -= (1LL << (8 * blen));
          }
          double d = schema.scale_ > 0 ? v / std::pow(10.0, schema.scale_) : v * 1.0;
          cell.set_double(d);
          pos += blen;
        } else {
          char *buf = static_cast<char *>(alloc.alloc(blen));
          if (OB_ISNULL(buf) && blen > 0) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, data + pos, blen);
            pos += blen;
            cell.set_varchar(ObString(blen, buf));
          }
        }
        break;
      }
      case 7: { // FIXED_LEN_BYTE_ARRAY
        int32_t tlen = schema.type_length_ > 0 ? schema.type_length_ : 0;
        if (tlen <= 0 || pos + tlen > len) {
          ret = OB_INVALID_ARGUMENT;
        } else if (5 == schema.converted_type_ || schema.logical_decimal_) {
          int64_t v = 0;
          bool neg = (tlen > 0) && (static_cast<uint8_t>(data[pos]) & 0x80);
          for (int64_t k = 0; k < tlen; ++k) {
            v = (v << 8) | static_cast<uint8_t>(data[pos + k]);
          }
          if (neg && tlen < 8) {
            v -= (1LL << (8 * tlen));
          }
          double d = schema.scale_ > 0 ? v / std::pow(10.0, schema.scale_) : v * 1.0;
          cell.set_double(d);
          pos += tlen;
        } else {
          char *buf = static_cast<char *>(alloc.alloc(tlen));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, data + pos, tlen);
            pos += tlen;
            cell.set_varchar(ObString(tlen, buf));
          }
        }
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        break;
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(out.push_back(cell))) {
        LOG_WARN("push cell failed", K(ret));
      }
    }
  }
  return ret;
}

static int64_t extract_parquet_kind(const ParquetSchemaElement &schema, bool *has_null)
{
  UNUSED(has_null);
  switch (schema.type_) {
    case 0:
      return FSQL_KIND_BOOL;
    case 1:
      if (6 == schema.converted_type_ || schema.logical_date_) {
        return FSQL_KIND_DATE;
      } else if (7 == schema.converted_type_ || schema.logical_time_has_unit_) {
        return FSQL_KIND_TIME;
      } else if (5 == schema.converted_type_ || schema.logical_decimal_) {
        return FSQL_KIND_DOUBLE;
      }
      return FSQL_KIND_INT;
    case 2:
      if (9 == schema.converted_type_ || 10 == schema.converted_type_ ||
          schema.logical_ts_unit_ > 0) {
        return FSQL_KIND_DATETIME;
      }
      if (schema.logical_time_has_unit_) {
        return FSQL_KIND_TIME;
      }
      if (5 == schema.converted_type_ || schema.logical_decimal_) {
        return FSQL_KIND_DOUBLE;
      }
      return FSQL_KIND_INT;
    case 3:
      return FSQL_KIND_DATETIME;
    case 4:
    case 5:
      return FSQL_KIND_DOUBLE;
    case 6:
      if (5 == schema.converted_type_ || schema.logical_decimal_) {
        return FSQL_KIND_DOUBLE;
      }
      return FSQL_KIND_VARCHAR;
    case 7:
      if (5 == schema.converted_type_ || schema.logical_decimal_) {
        return FSQL_KIND_DOUBLE;
      }
      return FSQL_KIND_VARCHAR;
    default:
      return FSQL_KIND_VARCHAR;
  }
}

} // end anonymous namespace

/////////////////////////////////////////////////////////////////////////////////
// public API
/////////////////////////////////////////////////////////////////////////////////

int ObFileScanUtils::open_secure_readonly(ObIAllocator &alloc, const ObString &path,
                                         const ObString &secure_file_priv, FILE *&file,
                                         ObString &canonical_path)
{
  int ret = OB_SUCCESS;
  int fd = -1;
  int dirfd = -1;
  file = nullptr;
  canonical_path.reset();
  try {
    if (path.empty() || secure_file_priv.empty() || ieq(secure_file_priv, "NULL") ||
        nullptr != memchr(path.ptr(), '\0', path.length()) ||
        nullptr != memchr(secure_file_priv.ptr(), '\0', secure_file_priv.length())) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else {
      const std::string requested(path.ptr(), path.length());
      const std::string allowed(secure_file_priv.ptr(), secure_file_priv.length());
      char root_buf[PATH_MAX + 1];
      char path_buf[PATH_MAX + 1];
      if (nullptr == realpath(allowed.c_str(), root_buf)) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else if (nullptr == realpath(requested.c_str(), path_buf)) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        const std::string root(root_buf);
        const std::string resolved(path_buf);
        const bool within = root == "/" ||
            (resolved.size() > root.size() && 0 == resolved.compare(0, root.size(), root)
             && resolved[root.size()] == '/');
        if (!within || resolved == "/") {
          ret = OB_ERR_NO_PRIVILEGE;
        } else {
          // Open every resolved component without following symlinks.  A
          // replacement between realpath and open therefore cannot escape
          // via a parent-directory symlink.
          dirfd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
          if (dirfd < 0) {
            ret = OB_IO_ERROR;
          } else {
            size_t start = 1;
            while (OB_SUCC(ret) && start < resolved.size()) {
              const size_t slash = resolved.find('/', start);
              const bool last = slash == std::string::npos;
              const std::string part = resolved.substr(start, last ? std::string::npos : slash - start);
              const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                  (last ? O_NONBLOCK : O_DIRECTORY);
              const int next = openat(dirfd, part.c_str(), flags);
              if (next < 0) {
                ret = OB_ERR_NO_PRIVILEGE;
              } else if (last) {
                fd = next;
              } else {
                ::close(dirfd);
                dirfd = next;
              }
              if (last) {
                break;
              }
              start = slash + 1;
            }
            ::close(dirfd);
            dirfd = -1;
          }
          if (OB_SUCC(ret)) {
            struct stat opened;
            struct stat current;
            if (fd < 0 || 0 != fstat(fd, &opened) || !S_ISREG(opened.st_mode) ||
                0 != stat(resolved.c_str(), &current) || opened.st_dev != current.st_dev ||
                opened.st_ino != current.st_ino) {
              ret = OB_ERR_NO_PRIVILEGE;
            } else if (OB_FAIL(copy_obstring(alloc, ObString(resolved.c_str()), canonical_path))) {
            } else if (nullptr == (file = fdopen(fd, "rb"))) {
              ret = OB_IO_ERROR;
            } else {
              fd = -1; // ownership transferred to FILE
            }
          }
        }
      }
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (fd >= 0) {
    ::close(fd);
  }
  if (dirfd >= 0) {
    ::close(dirfd);
  }
  return ret;
}

int ObFileScanUtils::file_identity(const struct stat &snapshot, ObIArray<int64_t> &identity)
{
  int ret = OB_SUCCESS;
  identity.reset();
#ifdef __APPLE__
  const int64_t values[] = {static_cast<int64_t>(snapshot.st_dev), static_cast<int64_t>(snapshot.st_ino),
    static_cast<int64_t>(snapshot.st_size), snapshot.st_mtimespec.tv_sec, snapshot.st_mtimespec.tv_nsec,
    snapshot.st_ctimespec.tv_sec, snapshot.st_ctimespec.tv_nsec};
#else
  const int64_t values[] = {static_cast<int64_t>(snapshot.st_dev), static_cast<int64_t>(snapshot.st_ino),
    static_cast<int64_t>(snapshot.st_size), snapshot.st_mtim.tv_sec, snapshot.st_mtim.tv_nsec,
    snapshot.st_ctim.tv_sec, snapshot.st_ctim.tv_nsec};
#endif
  for (int64_t value : values) {
    if (OB_FAIL(identity.push_back(value))) {
      break;
    }
  }
  return ret;
}

int ObFileScanUtils::glob_secure(ObIAllocator &alloc, const ObString &pattern,
                                 const ObString &secure_file_priv, ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  files.reset();
  try {
    if (pattern.empty() || secure_file_priv.empty() || ieq(secure_file_priv, "NULL") ||
        nullptr != memchr(pattern.ptr(), '\0', pattern.length()) ||
        nullptr != memchr(secure_file_priv.ptr(), '\0', secure_file_priv.length())) {
      return OB_ERR_NO_PRIVILEGE;
    }
    const std::string input(pattern.ptr(), pattern.length());
    const std::string configured(secure_file_priv.ptr(), secure_file_priv.length());
    const size_t wildcard = input.find_first_of("*?[");
    const size_t slash = input.rfind('/', wildcard == std::string::npos ? input.size() : wildcard);
    const std::string prefix = slash == std::string::npos ? "." : (slash == 0 ? "/" : input.substr(0, slash));
    const std::string suffix = slash == std::string::npos ? input : input.substr(slash + 1);
    char root_buf[PATH_MAX + 1];
    char base_buf[PATH_MAX + 1];
    if (nullptr == realpath(configured.c_str(), root_buf)) {
      return OB_ERR_NO_PRIVILEGE;
    } else if (nullptr == realpath(prefix.c_str(), base_buf)) {
      return OB_FILE_NOT_EXIST;
    }
    const std::string root(root_buf);
    const std::string base(base_buf);
    if (root != "/" && base != root &&
        !(base.size() > root.size() && 0 == base.compare(0, root.size(), root) && base[root.size()] == '/')) {
      return OB_ERR_NO_PRIVILEGE;
    }
    std::vector<std::string> segments;
    for (size_t start = 0; start < suffix.size();) {
      const size_t end = suffix.find('/', start);
      const std::string part = suffix.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (part.empty() || part == "." || part == ".." || segments.size() >= 32) {
        return OB_INVALID_ARGUMENT;
      }
      segments.push_back(part);
      if (end == std::string::npos) break;
      start = end + 1;
    }
    if (segments.empty()) return OB_INVALID_ARGUMENT;

    // Walk the canonical base from / without following a substituted symlink.
    struct ScopedFd {
      explicit ScopedFd(int fd) : fd_(fd) {}
      ~ScopedFd() { if (fd_ >= 0) ::close(fd_); }
      void reset(int fd) { if (fd_ >= 0) ::close(fd_); fd_ = fd; }
      int fd_;
    };
    ScopedFd base_fd(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (base_fd.fd_ < 0) return OB_IO_ERROR;
    for (size_t start = 1; OB_SUCC(ret) && start < base.size();) {
      const size_t end = base.find('/', start);
      const std::string part = base.substr(start, end == std::string::npos ? std::string::npos : end - start);
      const int next = openat(base_fd.fd_, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else {
        base_fd.reset(next);
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
    std::vector<std::string> found;
    int64_t visited = 0;
    std::function<int(int, const std::string &, size_t)> walk;
    walk = [&](int dirfd, const std::string &directory, size_t index) -> int {
      int local_ret = OB_SUCCESS;
      const int duplicate = dup(dirfd);
      if (duplicate < 0) return OB_IO_ERROR;
      DIR *raw_dir = fdopendir(duplicate);
      if (nullptr == raw_dir) { ::close(duplicate); return OB_IO_ERROR; }
      std::unique_ptr<DIR, decltype(&closedir)> dir(raw_dir, &closedir);
      errno = 0;
      while (OB_SUCC(local_ret)) {
        struct dirent *entry = readdir(dir.get());
        if (nullptr == entry) {
          if (errno != 0) local_ret = OB_IO_ERROR;
          break;
        }
        if (++visited > 1000000) return OB_SIZE_OVERFLOW;
        if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..") ||
            0 != fnmatch(segments[index].c_str(), entry->d_name, FNM_PERIOD)) {
          errno = 0;
          continue;
        }
        const bool last = index + 1 == segments.size();
        ScopedFd next(openat(dirfd, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
            (last ? O_NONBLOCK : O_DIRECTORY)));
        if (next.fd_ >= 0) {
          const std::string name = directory == "/" ? directory + entry->d_name : directory + "/" + entry->d_name;
          if (last) {
            struct stat info;
            if (0 == fstat(next.fd_, &info) && S_ISREG(info.st_mode)) {
              if (found.size() >= 10000) local_ret = OB_SIZE_OVERFLOW;
              else found.push_back(name);
            }
          } else {
            local_ret = walk(next.fd_, name, index + 1);
          }
        }
        errno = 0;
      }
      return local_ret;
    };
    if (OB_SUCC(ret)) ret = walk(base_fd.fd_, base, 0);
    if (OB_SUCC(ret)) {
      std::sort(found.begin(), found.end());
      for (const std::string &entry : found) {
        ObString copy;
        if (OB_FAIL(copy_obstring(alloc, ObString(entry.c_str()), copy))) {
          break;
        } else if (OB_FAIL(files.push_back(copy))) {
          break;
        }
      }
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObFileScanUtils::read_whole_file(ObIAllocator &alloc, const ObString &path,
                                     ObString &content)
{
  int ret = OB_SUCCESS;
  content.reset();
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file sql: empty path", K(ret));
  } else if (memchr(path.ptr(), '\0', path.length()) != nullptr) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file sql: path contains NUL", K(ret));
  } else {
    struct stat st;
    MEMSET(&st, 0, sizeof(st));
    char *pbuf = static_cast<char *>(alloc.alloc(path.length() + 1));
    if (OB_ISNULL(pbuf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(pbuf, path.ptr(), path.length());
      pbuf[path.length()] = '\0';
      if (0 != stat(pbuf, &st)) {
        ret = OB_FILE_NOT_EXIST;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: file does not exist");
        LOG_WARN("file sql: stat failed", K(ret), K(pbuf));
      } else if (!S_ISREG(st.st_mode)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: not a regular file");
        LOG_WARN("file sql: not a regular file", K(pbuf));
      } else if (st.st_size > OB_FILE_SQL_MAX_FILE_SIZE) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: file too large");
        LOG_WARN("file sql: file too large", K(pbuf), K(st.st_size));
      } else {
        FILE *fp = fopen(pbuf, "rb");
        if (OB_ISNULL(fp)) {
          ret = OB_FILE_NOT_EXIST;
          LOG_WARN("file sql: open failed", K(ret), K(pbuf));
        } else {
          int64_t sz = static_cast<int64_t>(st.st_size);
          char *buf = static_cast<char *>(alloc.alloc(sz + 1));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            int64_t got = fread(buf, 1, static_cast<size_t>(sz), fp);
            if (got != sz) {
              ret = OB_INVALID_ARGUMENT;
              LOG_WARN("file sql: short read", K(ret), K(pbuf), K(sz), K(got));
            } else {
              buf[sz] = '\0';
              content.assign_ptr(buf, static_cast<int32_t>(sz));
            }
          }
          fclose(fp);
        }
      }
    }
  }
  return ret;
}

int ObFileScanUtils::glob_files(ObIAllocator &alloc, const ObString &pattern,
                                ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  if (pattern.empty() || memchr(pattern.ptr(), '\0', pattern.length()) != nullptr) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file sql: invalid glob pattern", K(ret));
  } else {
    char *pbuf = static_cast<char *>(alloc.alloc(pattern.length() + 1));
    if (OB_ISNULL(pbuf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(pbuf, pattern.ptr(), pattern.length());
      pbuf[pattern.length()] = '\0';
      glob_t g;
      MEMSET(&g, 0, sizeof(g));
      int gr = glob(pbuf, GLOB_BRACE, nullptr, &g);
      if (0 != gr && GLOB_NOMATCH != gr) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("file sql: glob failed", K(ret), K(pbuf), K(gr));
      } else {
        for (size_t i = 0; OB_SUCC(ret) && i < g.gl_pathc; ++i) {
          ObString one;
          if (OB_FAIL(copy_obstring(alloc, ObString(g.gl_pathv[i]), one))) {
            LOG_WARN("copy glob result failed", K(ret));
          } else if (OB_FAIL(files.push_back(one))) {
            LOG_WARN("push glob result failed", K(ret));
          }
        }
      }
      // glob() may allocate part of its result even when it reports an error.
      globfree(&g);
    }
  }
  return ret;
}

int ObFileScanUtils::detect_value_kind(const ObString &text, int32_t &kind,
                                       int16_t &dt_scale)
{
  int ret = OB_SUCCESS;
  kind = FSQL_KIND_VARCHAR;
  dt_scale = 0;
  // SQL NULL is represented by a null ObObj before this helper is called.
  // A quoted empty CSV field is a non-NULL string and must widen the column.
  if (text.empty()) {
    kind = FSQL_KIND_VARCHAR;
  } else if (ieq(text, "true") || ieq(text, "false")) {
    kind = FSQL_KIND_BOOL;
  } else {
    int64_t iv = 0;
    double dv = 0;
    int16_t tscale = 0;
    int16_t dscale = 0;
    if (parse_int64_strict(text, iv)) {
      kind = FSQL_KIND_INT;
    } else if (looks_like_time(text, tscale)) {
      kind = FSQL_KIND_TIME;
    } else if (looks_like_date(text)) {
      // validate with the real converter
      int32_t d = 0;
      if (OB_SUCC(ObTimeConverter::str_to_date(text, d))) {
        kind = FSQL_KIND_DATE;
      } else {
        ret = OB_SUCCESS; // fall through to varchar
        kind = FSQL_KIND_VARCHAR;
      }
    } else if (looks_like_datetime(text, dscale)) {
      int64_t usec = 0;
      ObTimeConvertCtx cvrt_ctx(nullptr, false);
      ObString normalized(text);
      char localbuf[64];
      if (text.length() < 64 && text.ptr()[10] == 'T') {
        MEMCPY(localbuf, text.ptr(), text.length());
        localbuf[10] = ' ';
        normalized.assign_ptr(localbuf, text.length());
      }
      if (OB_SUCC(ObTimeConverter::str_to_datetime(normalized, cvrt_ctx, usec))) {
        kind = FSQL_KIND_DATETIME;
        dt_scale = dscale;
      } else {
        ret = OB_SUCCESS;
        kind = FSQL_KIND_VARCHAR;
      }
    } else if (parse_double_strict(text, dv)) {
      kind = FSQL_KIND_DOUBLE;
    } else if (is_int_overflow_double_candidate(text) && parse_double_strict(text, dv)) {
      kind = FSQL_KIND_DOUBLE;
    } else {
      kind = FSQL_KIND_VARCHAR;
    }
  }
  return ret;
}

int ObFileScanUtils::merge_kind(int32_t a, int32_t b, int32_t &merged)
{
  int ret = OB_SUCCESS;
  if (a == b || FSQL_KIND_NULL == b) {
    merged = a;
  } else if (FSQL_KIND_NULL == a) {
    merged = b;
  } else if ((FSQL_KIND_INT == a && FSQL_KIND_DOUBLE == b) ||
             (FSQL_KIND_DOUBLE == a && FSQL_KIND_INT == b)) {
    merged = FSQL_KIND_DOUBLE;
  } else if ((FSQL_KIND_DATE == a && FSQL_KIND_DATETIME == b) ||
             (FSQL_KIND_DATETIME == a && FSQL_KIND_DATE == b)) {
    merged = FSQL_KIND_DATETIME;
  } else {
    merged = FSQL_KIND_VARCHAR;
  }
  return ret;
}

const char *ObFileScanUtils::kind_type_name(int32_t kind)
{
  switch (kind) {
    case FSQL_KIND_BOOL:
      return "BOOLEAN";
    case FSQL_KIND_INT:
      return "BIGINT";
    case FSQL_KIND_DOUBLE:
      return "DOUBLE";
    case FSQL_KIND_TIME:
      return "TIME";
    case FSQL_KIND_DATE:
      return "DATE";
    case FSQL_KIND_DATETIME:
      return "DATETIME";
    default:
      return "VARCHAR";
  }
}

common::ObObjType ObFileScanUtils::kind_obj_type(int32_t kind)
{
  switch (kind) {
    case FSQL_KIND_BOOL:
      return ObTinyIntType;
    case FSQL_KIND_INT:
      return ObIntType;
    case FSQL_KIND_DOUBLE:
      return ObDoubleType;
    case FSQL_KIND_TIME:
      return ObTimeType;
    case FSQL_KIND_DATE:
      return ObDateType;
    case FSQL_KIND_DATETIME:
      return ObDateTimeType;
    default:
      return ObVarcharType;
  }
}

int ObFileScanUtils::cast_text_cell(ObIAllocator &alloc, const ObObj &cell,
                                    int32_t kind, ObObj &out)
{
  int ret = OB_SUCCESS;
  out.reset();
  if (cell.is_null()) {
    out.set_null();
  } else if (!cell.is_varchar() && !cell.is_string_type()) {
    out = cell;
  } else {
    const ObString &text = cell.get_string();
    switch (kind) {
      case FSQL_KIND_BOOL: {
        if (ieq(text, "true")) {
          out.set_tinyint(1);
        } else if (ieq(text, "false")) {
          out.set_tinyint(0);
        } else {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("file sql: invalid boolean", K(ret), K(text));
        }
        break;
      }
      case FSQL_KIND_INT: {
        int64_t v = 0;
        if (!parse_int64_strict(text, v)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("file sql: invalid integer", K(ret), K(text));
        } else {
          out.set_int(v);
        }
        break;
      }
      case FSQL_KIND_DOUBLE: {
        double v = 0;
        if (!parse_double_strict(text, v)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("file sql: invalid double", K(ret), K(text));
        } else {
          out.set_double(v);
        }
        break;
      }
      case FSQL_KIND_TIME: {
        int64_t v = 0;
        int16_t scale = 0;
        if (OB_FAIL(ObTimeConverter::str_to_time(text, v, &scale))) {
          LOG_WARN("file sql: invalid time", K(ret), K(text));
        } else {
          out.set_time(v);
        }
        break;
      }
      case FSQL_KIND_DATE: {
        int32_t d = 0;
        if (OB_FAIL(ObTimeConverter::str_to_date(text, d))) {
          LOG_WARN("file sql: invalid date", K(ret), K(text));
        } else {
          out.set_date(d);
        }
        break;
      }
      case FSQL_KIND_DATETIME: {
        int64_t usec = 0;
        ObTimeConvertCtx cvrt_ctx(nullptr, false);
        ObString normalized(text);
        char *localbuf = static_cast<char *>(alloc.alloc(text.length() + 1));
        if (OB_ISNULL(localbuf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(localbuf, text.ptr(), text.length());
          if (text.length() >= 11 && localbuf[10] == 'T') {
            localbuf[10] = ' ';
          }
          normalized.assign_ptr(localbuf, text.length());
          if (OB_FAIL(ObTimeConverter::str_to_datetime(normalized, cvrt_ctx, usec))) {
            LOG_WARN("file sql: invalid datetime", K(ret), K(text));
          } else {
            out.set_datetime(usec);
          }
        }
        break;
      }
      default: {
        out = cell;
        break;
      }
    }
  }
  return ret;
}

int ObFileScanUtils::csv_quote_field(ObIAllocator &alloc, const ObString &text,
                                     const ObString &delim, const ObString &quote,
                                     ObString &out)
{
  int ret = OB_SUCCESS;
  bool need_quote = false;
  for (int64_t i = 0; !need_quote && i < text.length(); ++i) {
    char c = text.ptr()[i];
    if ('\r' == c || '\n' == c) {
      need_quote = true;
    } else {
      for (int64_t d = 0; d < delim.length(); ++d) {
        if (c == delim.ptr()[d]) {
          need_quote = true;
          break;
        }
      }
      for (int64_t q = 0; !need_quote && q < quote.length(); ++q) {
        if (c == quote.ptr()[q]) {
          need_quote = true;
        }
      }
    }
  }
  if (!need_quote) {
    out = text;
  } else {
    int64_t qch = quote.length() > 0 ? quote.ptr()[0] : '"';
    int64_t cnt = 0;
    for (int64_t i = 0; i < text.length(); ++i) {
      if (text.ptr()[i] == qch) {
        ++cnt;
      }
    }
    int64_t total = text.length() + cnt + 2 * quote.length();
    char *buf = static_cast<char *>(alloc.alloc(total));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      MEMCPY(buf + pos, quote.ptr(), quote.length());
      pos += quote.length();
      for (int64_t i = 0; i < text.length(); ++i) {
        if (text.ptr()[i] == qch && cnt > 0) {
          buf[pos++] = static_cast<char>(qch);
        }
        buf[pos++] = text.ptr()[i];
      }
      MEMCPY(buf + pos, quote.ptr(), quote.length());
      pos += quote.length();
      out.assign_ptr(buf, static_cast<int32_t>(pos));
    }
  }
  return ret;
}

/////////////////////////////////////////////////////////////////////////////////
// CSV scanning
/////////////////////////////////////////////////////////////////////////////////

int ObFileScanUtils::scan_csv(ObIAllocator &alloc, const ObString &path,
                              const ObFileScanOptions &opts,
                              ObFileScanResult &result)
{
  int ret = OB_SUCCESS;
  ObString content;
  if (OB_FAIL(read_whole_file(alloc, path, content))) {
    LOG_WARN("file sql: read csv failed", K(ret));
  } else if (OB_FAIL(parse_csv_content(alloc, content, opts, result, false))) {
    LOG_WARN("file sql: parse csv failed", K(ret));
  }
  return ret;
}

int ObFileScanUtils::parse_csv_content(ObIAllocator &alloc, const ObString &content,
                                       const ObFileScanOptions &opts,
                                       ObFileScanResult &result, bool ndjson_like_name_fallback)
{
  int ret = OB_SUCCESS;
  UNUSED(ndjson_like_name_fallback);
  result.columns_.reset();
  result.cells_.reset();
  result.row_count_ = 0;
  const char *p = content.ptr();
  const int64_t n = content.length();
  char delim = opts.delim_.length() > 0 ? opts.delim_.ptr()[0] : ',';
  char quote = opts.quote_.length() > 0 ? opts.quote_.ptr()[0] : '"';
  char escape = opts.escape_.length() > 0 ? opts.escape_.ptr()[0] : quote;

  typedef ObSEArray<ObObj, 16> CellList;
  ObSEArray<CellList, 16> rows;

  // materialize the accumulated field bytes into a cell:
  // unquoted empty field or unquoted field equal to nullstr -> NULL
  auto finish_field = [&](ObSEArray<char, 64> &acc, bool field_quoted,
                          CellList &cells) -> int {
    int fret = OB_SUCCESS;
    ObObj cell;
    bool is_null = false;
    ObString raw(static_cast<int32_t>(acc.count()), acc.get_data());
    if (!field_quoted) {
      if (raw.length() == 0 && opts.nullstr_.empty()) {
        is_null = true;
      } else if (!opts.nullstr_.empty() && raw == opts.nullstr_) {
        is_null = true;
      }
    }
    if (is_null) {
      cell.set_null();
    } else {
      char *buf = static_cast<char *>(alloc.alloc(acc.count()));
      if (OB_ISNULL(buf) && acc.count() > 0) {
        fret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, acc.get_data(), acc.count());
        cell.set_varchar(ObString(static_cast<int32_t>(acc.count()), buf));
      }
    }
    if (OB_SUCC(fret) && OB_FAIL(cells.push_back(cell))) {
      fret = OB_ALLOCATE_MEMORY_FAILED;
    }
    return fret;
  };

  int64_t pos = 0;
  bool done = false;
  int64_t line_no = 0;
  while (OB_SUCC(ret) && !done) {
    // parse one record
    CellList cells;
    ObSEArray<char, 64> acc; // accumulates current field bytes
    bool field_quoted = false;
    bool in_quotes = false;
    bool field_started = false;
    bool saw_content = false;
    bool ended_by_newline = false;
    while (true) {
      if (pos >= n) {
        done = true;
        break;
      }
      char c = p[pos];
      if (in_quotes) {
        if (escape == quote && c == quote && pos + 1 < n && p[pos + 1] == quote) {
          acc.push_back(quote);
          pos += 2;
        } else if (escape != quote && c == escape && pos + 1 < n) {
          char e = p[pos + 1];
          switch (e) {
            case 'n': acc.push_back('\n'); break;
            case 'r': acc.push_back('\r'); break;
            case 't': acc.push_back('\t'); break;
            default: acc.push_back(e); break;
          }
          pos += 2;
        } else if (c == quote) {
          in_quotes = false;
          ++pos;
        } else {
          acc.push_back(c);
          ++pos;
        }
        saw_content = true;
        continue;
      }
      if (!field_started && c == quote) {
        in_quotes = true;
        field_quoted = true;
        field_started = true;
        saw_content = true;
        ++pos;
        continue;
      }
      if (c == delim) {
        ++pos;
        if (OB_FAIL(ret = finish_field(acc, field_quoted, cells))) {
          LOG_WARN("finish csv field failed", K(ret));
        } else {
          acc.reset();
          field_quoted = false;
          field_started = false;
          saw_content = true;
        }
        continue;
      }
      if (c == '\n' || c == '\r') {
        if (c == '\r' && pos + 1 < n && p[pos + 1] == '\n') {
          pos += 2;
        } else {
          pos += 1;
        }
        ended_by_newline = true;
        break;
      }
      acc.push_back(c);
      field_started = true;
      saw_content = true;
      ++pos;
    }
    if (OB_FAIL(ret)) {
    } else if (in_quotes) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: unterminated quote in csv");
      LOG_WARN("file sql: unterminated quote", K(ret));
    } else {
      // close the trailing field when the record carried anything
      bool has_trailing_field = ended_by_newline || !done || cells.count() > 0 ||
                                acc.count() > 0 || field_quoted || saw_content;
      if (has_trailing_field) {
        if (OB_FAIL(ret = finish_field(acc, field_quoted, cells))) {
          LOG_WARN("finish csv trailing field failed", K(ret));
        }
      }
      // a record consisting of a single unquoted empty field that came from a
      // bare newline (or a trailing newline at EOF) is a blank line: skip it
      bool blank_line = cells.count() == 1 && cells.at(0).is_null() && !saw_content;
      if (OB_SUCC(ret) && !blank_line && cells.count() > 0) {
        if (OB_FAIL(rows.push_back(cells))) {
          LOG_WARN("push csv row failed", K(ret));
        } else {
          ++line_no;
        }
      }
    }
    if (OB_SUCC(ret) && rows.count() > OB_FILE_SQL_MAX_ROWS) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: too many csv rows");
      LOG_WARN("file sql: too many rows", K(ret), K(rows.count()));
    }
  }

  // apply skip
  if (OB_SUCC(ret) && opts.skip_ > 0) {
    int64_t drop = std::min(opts.skip_, rows.count());
    for (int64_t i = 0; i < drop; ++i) {
      rows.at(i).reset();
    }
    // shift (ObSEArray has no erase; rebuild)
    ObSEArray<CellList, 16> kept;
    for (int64_t i = drop; OB_SUCC(ret) && i < rows.count(); ++i) {
      if (OB_FAIL(kept.push_back(rows.at(i)))) {
        LOG_WARN("shift rows failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      rows.reset();
      if (OB_FAIL(rows.assign(kept))) {
        LOG_WARN("assign rows failed", K(ret));
      }
    }
  }

  if (OB_SUCC(ret)) {
    ret = infer_columns_from_rows(alloc, rows, opts.header_, result);
  }
  return ret;
}

int ObFileScanUtils::infer_columns_from_rows(ObIAllocator &alloc,
                                             ObIArray<ObSEArray<ObObj, 16>> &rows,
                                             bool has_header_row,
                                             ObFileScanResult &result)
{
  int ret = OB_SUCCESS;
  result.columns_.reset();
  result.cells_.reset();
  result.row_count_ = 0;
  int64_t row_cnt = rows.count();
  int64_t data_start = has_header_row ? 1 : 0;
  if (0 == row_cnt) {
    // An empty file has no first row from which to infer columns.
    return OB_SUCCESS;
  }
  int64_t col_cnt = rows.at(0).count();
  if (col_cnt <= 0 || col_cnt > OB_FILE_SQL_MAX_COLUMNS) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: invalid column count");
    LOG_WARN("file sql: invalid column count", K(ret), K(col_cnt));
  } else {
    // column names
    ObSEArray<ObString, 16> names;
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      ObString name;
      if (has_header_row) {
        const ObObj &h = rows.at(0).at(i);
        if (h.is_null()) {
          // empty header falls back to positional name
        } else {
          name = h.get_string();
        }
      }
      if (name.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "column%ld", i);
        if (OB_FAIL(copy_obstring(alloc, ObString(buf), name))) {
          LOG_WARN("copy column name failed", K(ret));
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(names.push_back(name))) {
        LOG_WARN("push name failed", K(ret));
      }
    }
    // Ensure names stay unique even for three duplicates or an existing suffix.
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
        const ObString original = names.at(i);
        int64_t suffix = 1;
        bool unique = false;
        while (OB_SUCC(ret) && !unique) {
          ObSqlString generated;
          ObString candidate = original;
          if (suffix > 1) {
            if (OB_FAIL(generated.append_fmt("%.*s__%ld", original.length(), original.ptr(), suffix))) {
              LOG_WARN("format duplicate column name failed", K(ret));
            } else {
              candidate = generated.string();
            }
          }
          bool collision = false;
          for (int64_t j = 0; OB_SUCC(ret) && !collision && j < i; ++j) {
            collision = names.at(j).case_compare_equal(candidate);
          }
          if (OB_SUCC(ret) && !collision) {
            if (suffix > 1 && OB_FAIL(copy_obstring(alloc, candidate, names.at(i)))) {
              LOG_WARN("copy duplicate column name failed", K(ret));
            } else {
              unique = true;
            }
          } else if (OB_SUCC(ret)) {
            ++suffix;
          }
        }
      }
    }
    // consistency check + inference
    if (OB_SUCC(ret)) {
      ObSEArray<int32_t, 16> kinds;
      ObSEArray<int64_t, 16> nnvs;
      ObSEArray<int16_t, 16> scales;
      for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
        if (OB_FAIL(kinds.push_back(FSQL_KIND_NULL)) ||
            OB_FAIL(nnvs.push_back(0)) ||
            OB_FAIL(scales.push_back(0))) {
          LOG_WARN("init column state failed", K(ret));
        }
      }
      for (int64_t r = data_start; OB_SUCC(ret) && r < row_cnt; ++r) {
        if (rows.at(r).count() != col_cnt) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: ragged csv row");
          LOG_WARN("file sql: ragged row", K(ret), K(r), K(rows.at(r).count()), K(col_cnt));
        } else {
          for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
            const ObObj &cell = rows.at(r).at(c);
            if (cell.is_null()) {
              nnvs.at(c) = nnvs.at(c) + 1;
            } else {
              int32_t k = FSQL_KIND_VARCHAR;
              int16_t sc = 0;
              if (OB_FAIL(detect_value_kind(cell.get_string(), k, sc))) {
                LOG_WARN("detect kind failed", K(ret));
              } else {
                merge_kind(kinds.at(c), k, kinds.at(c));
                if (sc > scales.at(c)) {
                  scales.at(c) = sc;
                }
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
          if (FSQL_KIND_NULL == kinds.at(c)) {
            kinds.at(c) = FSQL_KIND_VARCHAR; // all-null column reads as VARCHAR
          }
          ObFileSqlColumn col;
          col.name_ = names.at(c);
          col.kind_ = kinds.at(c);
          col.null_count_ = nnvs.at(c);
          col.has_null_ = nnvs.at(c) > 0;
          col.datetime_scale_ = scales.at(c);
          if (OB_FAIL(result.columns_.push_back(col))) {
            LOG_WARN("push column meta failed", K(ret));
          }
        }
      }
      // materialize typed cells
      if (OB_SUCC(ret)) {
        for (int64_t r = data_start; OB_SUCC(ret) && r < row_cnt; ++r) {
          for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
            ObObj typed;
            if (OB_FAIL(cast_text_cell(alloc, rows.at(r).at(c), kinds.at(c), typed))) {
              LOG_WARN("cast cell failed", K(ret), K(r), K(c));
            } else if (OB_FAIL(result.cells_.push_back(typed))) {
              LOG_WARN("push typed cell failed", K(ret));
            }
          }
        }
        if (OB_SUCC(ret)) {
          result.row_count_ = row_cnt - data_start;
        }
      }
    }
  }
  return ret;
}

/////////////////////////////////////////////////////////////////////////////////
// JSON scanning
/////////////////////////////////////////////////////////////////////////////////

static int scan_json_object_row(ObIAllocator &alloc,
                                const rapidjson::Value &obj,
                                ObIArray<ObString> &names,
                                ObIArray<int32_t> &kinds,
                                ObIArray<int64_t> &nnvs,
                                ObIArray<int16_t> &scales,
                                ObFileScanResult &result,
                                int64_t row_idx_hint)
{
  int ret = OB_SUCCESS;
  if (!obj.IsObject()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: json record is not an object");
  } else {
    // union schema: first-seen order
    for (auto it = obj.MemberBegin(); OB_SUCC(ret) && it != obj.MemberEnd(); ++it) {
      ObString key(it->name.GetStringLength(), it->name.GetString());
      int64_t idx = -1;
      for (int64_t i = 0; i < names.count(); ++i) {
        if (names.at(i) == key) {
          idx = i;
          break;
        }
      }
      if (idx < 0) {
        ObString copied;
        if (OB_FAIL(copy_obstring(alloc, key, copied))) {
          LOG_WARN("copy json key failed", K(ret));
        } else if (OB_FAIL(names.push_back(copied)) ||
                   OB_FAIL(kinds.push_back(FSQL_KIND_NULL)) ||
                   OB_FAIL(nnvs.push_back(0)) ||
                   OB_FAIL(scales.push_back(0))) {
          LOG_WARN("push json column state failed", K(ret));
        } else {
          idx = names.count() - 1;
        }
      }
    }
    // second pass over columns: value or missing(=null)
    for (int64_t c = 0; OB_SUCC(ret) && c < names.count(); ++c) {
      rapidjson::Value::ConstMemberIterator mit = obj.FindMember(
          rapidjson::StringRef(names.at(c).ptr(), static_cast<rapidjson::SizeType>(names.at(c).length())));
      ObObj cell;
      if (mit == obj.MemberEnd()) {
        cell.set_null();
        nnvs.at(c) = nnvs.at(c) + 1;
      } else {
        const rapidjson::Value &v = mit->value;
        int32_t k = FSQL_KIND_VARCHAR;
        int16_t sc = 0;
        if (v.IsNull()) {
          cell.set_null();
          nnvs.at(c) = nnvs.at(c) + 1;
          k = FSQL_KIND_NULL;
        } else if (v.IsBool()) {
          cell.set_tinyint(v.GetBool() ? 1 : 0);
          k = FSQL_KIND_BOOL;
        } else if (v.IsInt64()) {
          cell.set_int(v.GetInt64());
          k = FSQL_KIND_INT;
        } else if (v.IsUint64()) {
          uint64_t u = v.GetUint64();
          if (u <= static_cast<uint64_t>(INT64_MAX)) {
            cell.set_int(static_cast<int64_t>(u));
            k = FSQL_KIND_INT;
          } else {
            cell.set_double(static_cast<double>(u));
            k = FSQL_KIND_DOUBLE;
          }
        } else if (v.IsDouble()) {
          cell.set_double(v.GetDouble());
          k = FSQL_KIND_DOUBLE;
        } else if (v.IsString()) {
          ObString s(static_cast<int32_t>(v.GetStringLength()), v.GetString());
          ObString copied;
          if (OB_FAIL(copy_obstring(alloc, s, copied))) {
            LOG_WARN("copy json string failed", K(ret));
          } else {
            cell.set_varchar(copied);
            k = FSQL_KIND_VARCHAR; // DuckDB json reader keeps strings VARCHAR
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql: nested JSON values");
        }
        if (OB_SUCC(ret)) {
          ObFileScanUtils::merge_kind(kinds.at(c), k, kinds.at(c));
          if (sc > scales.at(c)) {
            scales.at(c) = sc;
          }
        }
      }
      if (OB_SUCC(ret)) {
        // store in per-row staging (use result.cells_ later; stage into row map)
        if (OB_FAIL(result.cells_.push_back(cell))) {
          LOG_WARN("push json cell failed", K(ret));
        }
      }
    }
  }
  UNUSED(row_idx_hint);
  return ret;
}

// Values are gathered before the final union-column types are known.  Bring
// each value into that final type so a widened column never contains datums
// with a different physical type from its schema.
static int normalize_json_cell(ObIAllocator &alloc, int32_t kind, ObObj &cell)
{
  int ret = OB_SUCCESS;
  if (cell.is_null()) {
    // Missing and explicit null values retain their SQL NULL representation.
  } else if (FSQL_KIND_DOUBLE == kind && cell.is_int()) {
    const double value = static_cast<double>(cell.get_int());
    cell.set_double(value);
  } else if (FSQL_KIND_VARCHAR == kind && !cell.is_string_type()) {
    char buffer[128];
    int length = -1;
    if (cell.is_tinyint()) {
      const char *value = cell.get_tinyint() ? "true" : "false";
      length = snprintf(buffer, sizeof(buffer), "%s", value);
    } else if (cell.is_int()) {
      length = snprintf(buffer, sizeof(buffer), "%ld", cell.get_int());
    } else if (cell.is_double()) {
      length = snprintf(buffer, sizeof(buffer), "%.17g", cell.get_double());
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_SUCC(ret)) {
      ObString value;
      if (length < 0 || length >= static_cast<int>(sizeof(buffer))) {
        ret = OB_SIZE_OVERFLOW;
      } else if (OB_FAIL(copy_obstring(alloc, ObString(length, buffer), value))) {
      } else {
        cell.set_varchar(value);
      }
    }
  }
  return ret;
}

int ObFileScanUtils::parse_json_content(ObIAllocator &alloc, const ObString &content,
                                        const ObString &explicit_format,
                                        ObFileScanResult &result)
{
  int ret = OB_SUCCESS;
  result.columns_.reset();
  result.cells_.reset();
  result.row_count_ = 0;
  bool is_ndjson = false;
  bool is_array = false;
  if (explicit_format.empty() || ieq(explicit_format, "auto")) {
    // auto detect: parse as a single json array; otherwise newline-delimited
    rapidjson::Document doc;
    doc.Parse(content.ptr(), static_cast<rapidjson::SizeType>(content.length()));
    if (!doc.HasParseError() && doc.IsArray()) {
      is_array = true;
    } else {
      is_ndjson = true;
    }
  } else if (ieq(explicit_format, "newline_delimited")) {
    is_ndjson = true;
  } else if (ieq(explicit_format, "array")) {
    is_array = true;
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: unsupported json format");
    LOG_WARN("file sql: unsupported json format", K(ret), K(explicit_format));
  }

  ObSEArray<ObString, 16> names;
  ObSEArray<int32_t, 16> kinds;
  ObSEArray<int64_t, 16> nnvs;
  ObSEArray<int16_t, 16> scales;
  int64_t rows = 0;
  if (OB_SUCC(ret) && is_array) {
    rapidjson::Document doc;
    doc.Parse(content.ptr(), static_cast<rapidjson::SizeType>(content.length()));
    if (doc.HasParseError() || !doc.IsArray()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: invalid json array file");
      LOG_WARN("file sql: invalid json array", K(ret), K(doc.GetParseError()));
    } else {
      // NOTE: each record must widen the union schema; cells for earlier rows
      // were staged with the then-known column set. To keep row-major output
      // correct we do two passes over the array.
      // pass 1: schema
      for (rapidjson::SizeType i = 0; OB_SUCC(ret) && i < doc.Size(); ++i) {
        const rapidjson::Value &obj = doc[i];
        if (!obj.IsObject()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: json record is not an object");
        } else {
          for (auto it = obj.MemberBegin(); OB_SUCC(ret) && it != obj.MemberEnd(); ++it) {
            ObString key(it->name.GetStringLength(), it->name.GetString());
            bool found = false;
            for (int64_t c = 0; c < names.count(); ++c) {
              if (names.at(c) == key) {
                found = true;
                break;
              }
            }
            if (!found) {
              ObString copied;
              if (OB_FAIL(copy_obstring(alloc, key, copied))) {
              } else if (OB_FAIL(names.push_back(copied)) ||
                         OB_FAIL(kinds.push_back(FSQL_KIND_NULL)) ||
                         OB_FAIL(nnvs.push_back(0)) ||
                         OB_FAIL(scales.push_back(0))) {
                LOG_WARN("push json column failed", K(ret));
              }
            }
          }
        }
      }
      // pass 2: values
      for (rapidjson::SizeType i = 0; OB_SUCC(ret) && i < doc.Size(); ++i) {
        if (OB_FAIL(scan_json_object_row(alloc, doc[i], names, kinds, nnvs, scales,
                                         result, i))) {
          LOG_WARN("scan json array row failed", K(ret), K(i));
        } else {
          ++rows;
        }
      }
    }
  } else if (OB_SUCC(ret) && is_ndjson) {
    // two passes as well: first collect schema, then values
    const char *p = content.ptr();
    int64_t n = content.length();
    int64_t pos = 0;
    rapidjson::Document probe;
    while (OB_SUCC(ret) && pos < n) {
      int64_t eol = pos;
      while (eol < n && p[eol] != '\n') {
        ++eol;
      }
      int64_t lend = eol;
      if (lend > pos && p[lend - 1] == '\r') {
        --lend;
      }
      if (lend > pos) {
        probe.Parse(p + pos, static_cast<rapidjson::SizeType>(lend - pos));
        if (probe.HasParseError() || !probe.IsObject()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: invalid jsonl line");
          LOG_WARN("file sql: invalid jsonl line", K(ret));
        } else {
          for (auto it = probe.MemberBegin(); OB_SUCC(ret) && it != probe.MemberEnd(); ++it) {
            ObString key(it->name.GetStringLength(), it->name.GetString());
            bool found = false;
            for (int64_t c = 0; c < names.count(); ++c) {
              if (names.at(c) == key) {
                found = true;
                break;
              }
            }
            if (!found) {
              ObString copied;
              if (OB_FAIL(copy_obstring(alloc, key, copied))) {
              } else if (OB_FAIL(names.push_back(copied)) ||
                         OB_FAIL(kinds.push_back(FSQL_KIND_NULL)) ||
                         OB_FAIL(nnvs.push_back(0)) ||
                         OB_FAIL(scales.push_back(0))) {
                LOG_WARN("push jsonl column failed", K(ret));
              }
            }
          }
        }
      }
      pos = eol + 1;
    }
    // pass 2
    pos = 0;
    while (OB_SUCC(ret) && pos < n) {
      int64_t eol = pos;
      while (eol < n && p[eol] != '\n') {
        ++eol;
      }
      int64_t lend = eol;
      if (lend > pos && p[lend - 1] == '\r') {
        --lend;
      }
      if (lend > pos) {
        rapidjson::Document doc;
        doc.Parse(p + pos, static_cast<rapidjson::SizeType>(lend - pos));
        if (doc.HasParseError() || !doc.IsObject()) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(scan_json_object_row(alloc, doc, names, kinds, nnvs, scales,
                                                result, rows))) {
          LOG_WARN("scan jsonl row failed", K(ret));
        } else {
          ++rows;
        }
      }
      pos = eol + 1;
    }
  }
  if (OB_SUCC(ret)) {
    if (rows > OB_FILE_SQL_MAX_ROWS) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t c = 0; OB_SUCC(ret) && c < names.count(); ++c) {
      if (FSQL_KIND_NULL == kinds.at(c)) {
        kinds.at(c) = FSQL_KIND_VARCHAR;
      }
      ObFileSqlColumn col;
      col.name_ = names.at(c);
      col.kind_ = kinds.at(c);
      col.null_count_ = nnvs.at(c);
      col.has_null_ = nnvs.at(c) > 0 || rows == 0;
      col.datetime_scale_ = scales.at(c);
      if (OB_FAIL(result.columns_.push_back(col))) {
        LOG_WARN("push json column meta failed", K(ret));
      }
    }
    for (int64_t row = 0; OB_SUCC(ret) && row < rows; ++row) {
      for (int64_t col = 0; OB_SUCC(ret) && col < names.count(); ++col) {
        if (OB_FAIL(normalize_json_cell(alloc, kinds.at(col),
                                        result.cells_.at(row * names.count() + col)))) {
          LOG_WARN("normalize widened json cell failed", K(ret), K(row), K(col));
        }
      }
    }
    result.row_count_ = rows;
  }
  return ret;
}

int ObFileScanUtils::scan_json(ObIAllocator &alloc, const ObString &path,
                               const ObFileScanOptions &opts,
                               ObFileScanResult &result)
{
  int ret = OB_SUCCESS;
  ObString content;
  if (OB_FAIL(read_whole_file(alloc, path, content))) {
    LOG_WARN("file sql: read json failed", K(ret));
  } else if (OB_FAIL(parse_json_content(alloc, content, opts.format_, result))) {
    LOG_WARN("file sql: parse json failed", K(ret));
  }
  return ret;
}

int ObFileScanUtils::scan_ndjson(ObIAllocator &alloc, const ObString &path,
                                 const ObFileScanOptions &opts,
                                 ObFileScanResult &result)
{
  int ret = OB_SUCCESS;
  UNUSED(opts);
  ObString content;
  if (OB_FAIL(read_whole_file(alloc, path, content))) {
    LOG_WARN("file sql: read ndjson failed", K(ret));
  } else if (OB_FAIL(parse_json_content(alloc, content,
                                        ObString("newline_delimited"), result))) {
    LOG_WARN("file sql: parse ndjson failed", K(ret));
  }
  return ret;
}

/////////////////////////////////////////////////////////////////////////////////
// parquet scanning
/////////////////////////////////////////////////////////////////////////////////

int ObFileScanUtils::scan_parquet(ObIAllocator &alloc, const ObString &path,
                                  ObFileScanResult &result)
{
  int ret = OB_SUCCESS;
  ObString content;
  result.columns_.reset();
  result.cells_.reset();
  result.row_count_ = 0;
  if (OB_FAIL(read_whole_file(alloc, path, content))) {
    LOG_WARN("file sql: read parquet failed", K(ret));
  } else if (content.length() < 12 ||
             0 != MEMCMP(content.ptr(), "PAR1", 4) ||
             0 != MEMCMP(content.ptr() + content.length() - 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: invalid parquet file");
    LOG_WARN("file sql: bad parquet magic", K(ret));
  } else {
    const char *base = content.ptr();
    int64_t len = content.length();
    int32_t meta_len = 0;
    for (int i = 0; i < 4; ++i) {
      meta_len |= static_cast<int32_t>(static_cast<uint8_t>(base[len - 8 + i])) << (8 * i);
    }
    if (meta_len <= 0 || meta_len > len - 12) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("file sql: bad parquet footer length", K(ret), K(meta_len));
    } else {
      ThriftReader r(base + len - 8 - meta_len, meta_len);
      // FileMetaData: 1 version, 2 schema, 3 num_rows, 4 row_groups, 5 kv, 6 created_by
      ObSEArray<ParquetSchemaElement, 16> schema;
      ObSEArray<ParquetColumnChunkMeta, 16> chunks;
      ObSEArray<int64_t, 16> rg_num_rows;
      int16_t fid = 0;
      int16_t last_fid = 0;
      int8_t ftype = 0;
      bool stop = false;
      while (OB_SUCC(ret) && !stop) {
        if (OB_FAIL(r.read_field_begin(ftype, fid, stop, last_fid))) {
        } else if (stop) {
          break;
        } else if (fid == 2 && 9 == ftype) {
          int8_t etype = 0;
          int64_t cnt = 0;
          if (OB_FAIL(r.read_list_begin(etype, cnt)) || cnt <= 0 || cnt > OB_FILE_SQL_MAX_COLUMNS + 1) {
            if (OB_SUCC(ret)) {
              ret = OB_INVALID_ARGUMENT;
            }
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
              ParquetSchemaElement e;
              if (OB_FAIL(parse_parquet_schema_element(r, alloc, e))) {
                LOG_WARN("parse schema element failed", K(ret), K(i));
              } else if (OB_FAIL(schema.push_back(e))) {
                LOG_WARN("push schema element failed", K(ret));
              }
            }
          }
        } else if (fid == 4 && 9 == ftype) {
          int8_t etype = 0;
          int64_t cnt = 0;
          if (OB_FAIL(r.read_list_begin(etype, cnt)) || cnt < 0) {
            if (OB_SUCC(ret)) {
              ret = OB_INVALID_ARGUMENT;
            }
          } else {
            for (int64_t g = 0; OB_SUCC(ret) && g < cnt; ++g) {
              // RowGroup {1: columns list, 2: total_byte_size, 3: num_rows}
              int16_t fid2 = 0, last2 = 0;
              int8_t ft2 = 0;
              bool stop2 = false;
              int64_t num_rows = 0;
              while (OB_SUCC(ret) && !stop2) {
                if (OB_FAIL(r.read_field_begin(ft2, fid2, stop2, last2))) {
                } else if (stop2) {
                  break;
                } else if (fid2 == 1 && 9 == ft2) {
                  int8_t e2 = 0;
                  int64_t c2 = 0;
                  if (OB_FAIL(r.read_list_begin(e2, c2))) {
                  } else {
                    for (int64_t k = 0; OB_SUCC(ret) && k < c2; ++k) {
                      ParquetColumnChunkMeta m;
                      if (OB_FAIL(parse_parquet_column_meta(r, alloc, m))) {
                        LOG_WARN("parse column chunk failed", K(ret));
                      } else if (OB_FAIL(chunks.push_back(m))) {
                        LOG_WARN("push column chunk failed", K(ret));
                      }
                    }
                  }
                } else if (fid2 == 3) {
                  ret = r.read_i64(num_rows);
                } else {
                  ret = r.skip_value(ft2);
                }
              }
              if (OB_SUCC(ret) && OB_FAIL(rg_num_rows.push_back(num_rows))) {
                LOG_WARN("push rg rows failed", K(ret));
              }
            }
          }
        } else {
          ret = r.skip_value(ftype);
        }
      }
      if (OB_SUCC(ret)) {
        if (schema.count() < 2 || 0 == rg_num_rows.count()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("file sql: parquet missing schema or row groups", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        // flat schema requirement: root has num_children = N, all children have
        // num_children == 0 (or absent)
        const ParquetSchemaElement &root = schema.at(0);
        int64_t col_cnt = root.num_children_;
        if (col_cnt != schema.count() - 1) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested parquet schema");
          LOG_WARN("file sql: nested parquet schema not supported", K(ret), K(col_cnt));
        } else {
          int64_t total_rows = 0;
          for (int64_t g = 0; g < rg_num_rows.count(); ++g) {
            total_rows += rg_num_rows.at(g);
            if (rg_num_rows.at(g) < 0) {
              ret = OB_INVALID_ARGUMENT;
              break;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (total_rows > OB_FILE_SQL_MAX_ROWS) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            // column-major decode into per-column vectors
            ObSEArray<ObSEArray<ObObj, 64>, 16> colvals;
            for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
              ObSEArray<ObObj, 64> vec;
              const ParquetSchemaElement &se = schema.at(c + 1);
              if (OB_FAIL(vec.reserve(64))) {
              }
              // find chunks for this column (one per row group)
              for (int64_t g = 0; OB_SUCC(ret) && g < rg_num_rows.count(); ++g) {
                const int64_t group_start_count = vec.count();
                // chunk index = g * col_cnt + c assuming order; safer: search by name+order
                int64_t found = -1;
                int64_t seen = 0;
                for (int64_t k = 0; k < chunks.count(); ++k) {
                  if (chunks.at(k).column_name_ == se.name_) {
                    if (seen == g) {
                      found = k;
                      break;
                    }
                    ++seen;
                  }
                }
                if (found < 0) {
                  ret = OB_INVALID_ARGUMENT;
                  LOG_WARN("file sql: parquet column chunk missing", K(ret), K(se.name_));
                } else {
                  const ParquetColumnChunkMeta &m = chunks.at(found);
                  int64_t start = m.dictionary_page_offset_ >= 0
                                      ? (m.dictionary_page_offset_ < m.data_page_offset_
                                             ? m.dictionary_page_offset_
                                             : m.data_page_offset_)
                                      : m.data_page_offset_;
                  if (start < 0 || start >= len) {
                    ret = OB_INVALID_ARGUMENT;
                    LOG_WARN("file sql: bad page offset", K(ret), K(start));
                  } else {
                    // read pages until we have m.num_values_ values
                    ParquetDict dict;
                    int64_t got = 0;
                    int64_t p = start;
                    while (OB_SUCC(ret) && got < m.num_values_ && p < len) {
                      ThriftReader pr(base + p, len - p);
                      ParquetPageHeader ph;
                      int64_t header_start = p;
                      if (OB_FAIL(parse_parquet_page_header(pr, ph))) {
                        LOG_WARN("parse page header failed", K(ret), K(p));
                      } else {
                        int64_t data_off = header_start + pr.pos();
                        int64_t comp_len = ph.compressed_size_;
                        if (comp_len < 0 || ph.uncompressed_size_ < 0 ||
                            ph.uncompressed_size_ > OB_FILE_SQL_MAX_FILE_SIZE || data_off + comp_len > len) {
                          ret = OB_INVALID_ARGUMENT;
                        } else {
                          const char *page_data = base + data_off;
                          p = data_off + comp_len;
                          if (ph.type_ == 2) {
                            // dictionary page: decompress + plain decode
                            char *dst = static_cast<char *>(alloc.alloc(ph.uncompressed_size_));
                            int64_t outlen = 0;
                            if (OB_ISNULL(dst) && ph.uncompressed_size_ > 0) {
                              ret = OB_ALLOCATE_MEMORY_FAILED;
                            } else if (OB_FAIL(parquet_decompress(m.codec_, page_data, comp_len,
                                                                  dst, ph.uncompressed_size_, outlen))) {
                              LOG_WARN("decompress dict page failed", K(ret), K(m.codec_));
                            } else if (OB_FAIL(decode_plain_values(alloc, dst, outlen, se,
                                                                   se.type_, ph.num_values_,
                                                                   dict.values_))) {
                              LOG_WARN("decode dict page failed", K(ret));
                            } else {
                              dict.valid_ = true;
                            }
                          } else if (ph.type_ == 0 || ph.type_ == 3) {
                            // data page
                            char *dst = static_cast<char *>(alloc.alloc(ph.uncompressed_size_ + 1));
                            if (OB_ISNULL(dst)) {
                              ret = OB_ALLOCATE_MEMORY_FAILED;
                            } else {
                              int64_t outlen = 0;
                              int64_t values_len = comp_len;
                              int64_t def_bytes_len = 0;
                              const char *def_data = nullptr;
                              const char *values_data = page_data;
                              if (ph.type_ == 3) {
                                // v2: levels are stored uncompressed at the
                                // front; only value bytes are compressed
                                def_bytes_len = ph.v2_def_len_;
                                values_len = comp_len - ph.v2_def_len_ - ph.v2_rep_len_;
                                def_data = page_data + ph.v2_rep_len_;
                                values_data = page_data + ph.v2_def_len_ + ph.v2_rep_len_;
                                if (values_len < 0) {
                                  ret = OB_INVALID_ARGUMENT;
                                } else if (!ph.v2_is_compressed_) {
                                  if (OB_FAIL(parquet_decompress(0, values_data, values_len, dst,
                                                                 ph.uncompressed_size_, outlen))) {
                                    // 0 = memcpy
                                  }
                                } else if (OB_FAIL(parquet_decompress(m.codec_, values_data,
                                                                     values_len, dst,
                                                                     ph.uncompressed_size_, outlen))) {
                                  LOG_WARN("decompress v2 page failed", K(ret), K(m.codec_));
                                }
                                values_data = dst;
                              } else {
                                // V1 compresses the entire page, including the
                                // length-prefixed definition-level stream.
                                int64_t max_def = (1 == se.repetition_type_) ? 1 : 0;
                                if (OB_FAIL(parquet_decompress(m.codec_, page_data, comp_len,
                                                               dst, ph.uncompressed_size_, outlen))) {
                                  LOG_WARN("decompress v1 page failed", K(ret), K(m.codec_));
                                } else if (max_def > 0) {
                                  if (outlen < 4) {
                                    ret = OB_INVALID_ARGUMENT;
                                  } else {
                                    int32_t rle_len = 0;
                                    for (int i = 0; i < 4; ++i) {
                                      rle_len |= static_cast<int32_t>(
                                          static_cast<uint8_t>(dst[i])) << (8 * i);
                                    }
                                    if (rle_len < 0 || 4 + rle_len > outlen) {
                                      ret = OB_INVALID_ARGUMENT;
                                    } else {
                                      def_data = dst + 4;
                                      def_bytes_len = rle_len;
                                      values_data = dst + 4 + rle_len;
                                      outlen -= 4 + rle_len;
                                    }
                                  }
                                } else {
                                  values_data = dst;
                                }
                              }
                              if (OB_SUCC(ret)) {
                                // definition levels
                                int64_t max_def = (1 == se.repetition_type_) ? 1 : 0;
                                ObSEArray<int64_t, 64> defs;
                                if (max_def > 0) {
                                  const char *lvl = (ph.type_ == 3) ? def_data : def_data;
                                  int64_t lvl_len = (ph.type_ == 3) ? ph.v2_def_len_ : def_bytes_len;
                                  if (OB_ISNULL(lvl) || lvl_len <= 0) {
                                    ret = OB_INVALID_ARGUMENT;
                                  } else {
                                    // v2: levels not length-prefixed; v1: skip
                                    // the 4-byte length already
                                    RleDecoder rd(lvl, lvl_len, 1);
                                    for (int64_t i = 0; OB_SUCC(ret) && i < ph.num_values_; ++i) {
                                      int64_t d = 0;
                                      int rr = rd.next(d);
                                      if (OB_ITER_END == rr) {
                                        ret = OB_INVALID_ARGUMENT;
                                      } else if (OB_FAIL(rr)) {
                                        LOG_WARN("decode def levels failed", K(ret));
                                      }
                                      if (OB_SUCC(ret) && OB_FAIL(defs.push_back(d))) {
                                      }
                                    }
                                  }
                                }
                                if (OB_SUCC(ret)) {
                                  // values
                                  if (8 == ph.encoding_ || 2 == ph.encoding_) {
                                    // RLE_DICTIONARY / PLAIN_DICTIONARY
                                    if (!dict.valid_ || outlen < 1) {
                                      ret = OB_INVALID_ARGUMENT;
                                      LOG_WARN("file sql: dictionary missing for page", K(ret));
                                    } else {
                                      int32_t bw = static_cast<uint8_t>(values_data[0]);
                                      RleDecoder rd(values_data + 1, outlen - 1, bw);
                                      for (int64_t i = 0; OB_SUCC(ret) && i < ph.num_values_; ++i) {
                                        bool is_null = (defs.count() == ph.num_values_) &&
                                                       (defs.at(i) < max_def);
                                        if (is_null) {
                                          ObObj nullcell;
                                          nullcell.set_null();
                                          if (OB_FAIL(vec.push_back(nullcell))) {
                                          }
                                        } else {
                                          int64_t idx = 0;
                                          if (OB_FAIL(rd.next(idx))) {
                                            LOG_WARN("decode dict idx failed", K(ret));
                                          } else if (idx < 0 ||
                                                     idx >= dict.values_.count()) {
                                            ret = OB_INVALID_ARGUMENT;
                                          } else if (OB_FAIL(vec.push_back(dict.values_.at(idx)))) {
                                          }
                                        }
                                      }
                                    }
                                  } else if (0 == ph.encoding_) {
                                    // PLAIN; count non-null values
                                    int64_t non_null = 0;
                                    for (int64_t i = 0; i < defs.count() && i < ph.num_values_; ++i) {
                                      if (defs.at(i) >= max_def) {
                                        ++non_null;
                                      }
                                    }
                                    if (defs.count() == 0) {
                                      non_null = ph.num_values_;
                                    }
                                    ObSEArray<ObObj, 64> plain_vals;
                                    if (OB_FAIL(decode_plain_values(alloc, values_data, outlen, se,
                                                                    se.type_, non_null,
                                                                    plain_vals))) {
                                      LOG_WARN("decode plain page failed", K(ret));
                                    } else {
                                      int64_t vi = 0;
                                      for (int64_t i = 0; OB_SUCC(ret) && i < ph.num_values_; ++i) {
                                        bool is_null = (defs.count() == ph.num_values_) &&
                                                       (defs.at(i) < max_def);
                                        if (is_null) {
                                          ObObj nullcell;
                                          nullcell.set_null();
                                          if (OB_FAIL(vec.push_back(nullcell))) {
                                          }
                                        } else if (vi < plain_vals.count()) {
                                          if (OB_FAIL(vec.push_back(plain_vals.at(vi)))) {
                                          }
                                          ++vi;
                                        } else {
                                          ret = OB_INVALID_ARGUMENT;
                                        }
                                      }
                                    }
                                  } else if (3 == ph.encoding_ && 0 == se.type_) {
                                    // BOOLEAN RLE values carry a four-byte
                                    // encoded-stream length before the hybrid runs.
                                    if (outlen < 4) {
                                      ret = OB_INVALID_ARGUMENT;
                                    } else {
                                      uint32_t encoded_len = 0;
                                      for (int i = 0; i < 4; ++i) {
                                        encoded_len |= static_cast<uint32_t>(
                                            static_cast<uint8_t>(values_data[i])) << (8 * i);
                                      }
                                      if (encoded_len != static_cast<uint64_t>(outlen - 4)) {
                                        ret = OB_INVALID_ARGUMENT;
                                      } else {
                                        RleDecoder rd(values_data + 4, encoded_len, 1);
                                        for (int64_t i = 0; OB_SUCC(ret) && i < ph.num_values_; ++i) {
                                          ObObj cell;
                                          if (defs.count() == ph.num_values_ && defs.at(i) < max_def) {
                                            cell.set_null();
                                          } else {
                                            int64_t value = 0;
                                            if (OB_FAIL(rd.next(value))) {
                                              if (OB_ITER_END == ret) ret = OB_INVALID_ARGUMENT;
                                            } else {
                                              cell.set_tinyint(value ? 1 : 0);
                                            }
                                          }
                                          if (OB_SUCC(ret) && OB_FAIL(vec.push_back(cell))) {
                                          }
                                        }
                                      }
                                    }
                                  } else {
                                    ret = OB_NOT_SUPPORTED;
                                    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                                                   "parquet encoding");
                                    LOG_WARN("file sql: unsupported parquet encoding",
                                             K(ret), K(ph.encoding_));
                                  }
                                }
                              }
                            }
                          } else {
                            // index page or unknown: skip
                          }
                        }
                        if (OB_SUCC(ret) && (ph.type_ == 0 || ph.type_ == 3)) {
                          got += ph.num_values_;
                        }
                      }
                    }
                    if (OB_SUCC(ret) && vec.count() - group_start_count != rg_num_rows.at(g)) {
                      ret = OB_INVALID_ARGUMENT;
                      LOG_WARN("file sql: parquet row group value count mismatch", K(ret), K(g));
                    }
                  }
                }
              }
              if (OB_SUCC(ret) && vec.count() != total_rows) {
                ret = OB_INVALID_ARGUMENT;
              } else if (OB_SUCC(ret) && OB_FAIL(colvals.push_back(vec))) {
                LOG_WARN("push col vals failed", K(ret));
              }
            }
            // row-major materialize + column meta
            if (OB_SUCC(ret)) {
              for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
                const ParquetSchemaElement &se = schema.at(c + 1);
                ObFileSqlColumn col;
                col.name_ = se.name_;
                col.kind_ = static_cast<int32_t>(extract_parquet_kind(se, nullptr));
                int64_t nnv = 0;
                for (int64_t i = 0; i < colvals.at(c).count(); ++i) {
                  if (colvals.at(c).at(i).is_null()) {
                    ++nnv;
                  }
                }
                col.null_count_ = nnv;
                col.has_null_ = nnv > 0;
                col.datetime_scale_ = 6; // parquet timestamps carry micros
                if (FSQL_KIND_DATETIME != col.kind_ && FSQL_KIND_TIME != col.kind_) {
                  col.datetime_scale_ = 0;
                }
                if (OB_FAIL(result.columns_.push_back(col))) {
                  LOG_WARN("push parquet column failed", K(ret));
                }
              }
              for (int64_t i = 0; OB_SUCC(ret) && i < total_rows; ++i) {
                for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
                  if (OB_FAIL(result.cells_.push_back(colvals.at(c).at(i)))) {
                    LOG_WARN("push parquet cell failed", K(ret));
                  }
                }
              }
              if (OB_SUCC(ret)) {
                result.row_count_ = total_rows;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

FileCsvStream::FileCsvStream()
  : file_(nullptr), delim_(','), quote_('"'), escape_('"'), header_(true),
    skip_(0), row_count_(0), data_offset_(0), snapshot_{}
{}

FileCsvStream::~FileCsvStream()
{
  close();
}

void FileCsvStream::close()
{
  if (nullptr != file_) {
    fclose(file_);
    file_ = nullptr;
  }
  path_.clear();
  columns_.clear();
  row_count_ = 0;
  data_offset_ = 0;
}

int FileCsvStream::init(FILE *file, const ObString &path, const ObFileScanOptions &opts)
{
  int ret = OB_SUCCESS;
  close();
  file_ = file;
  try {
    if (nullptr == file_ || path.empty() || nullptr != memchr(path.ptr(), '\0', path.length()) ||
        opts.delim_.length() != 1 || opts.quote_.length() != 1 || opts.escape_.length() != 1 ||
        opts.delim_.ptr()[0] == opts.quote_.ptr()[0] || opts.skip_ < 0 || opts.sample_size_ != -1) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      path_.assign(path.ptr(), path.length());
      nullstr_.assign(opts.nullstr_.ptr() ? opts.nullstr_.ptr() : "", opts.nullstr_.length());
      delim_ = opts.delim_.ptr()[0];
      quote_ = opts.quote_.ptr()[0];
      escape_ = opts.escape_.ptr()[0];
      header_ = opts.header_;
      skip_ = opts.skip_;
      if (0 != fstat(fileno(file_), &snapshot_) || !S_ISREG(snapshot_.st_mode)) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(verify_unchanged())) {
      } else if (OB_FAIL(infer_schema())) {
      }
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

static int verify_file_snapshot(FILE *file, const std::string &path, const struct stat &snapshot)
{
  int ret = OB_SUCCESS;
  struct stat fd_stat;
  struct stat path_stat;
  if (nullptr == file || 0 != fstat(fileno(file), &fd_stat) ||
      0 != stat(path.c_str(), &path_stat)) {
    ret = OB_IO_ERROR;
  } else {
    auto same = [](const struct stat &a, const struct stat &b) {
#ifdef __APPLE__
      const bool same_time = a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
                             a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
                             a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
                             a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
      const bool same_time = a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
                             a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
                             a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
                             a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
      return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && same_time;
    };
    if (!same(snapshot, fd_stat) || !same(snapshot, path_stat)) {
      ret = OB_IO_ERROR;
      LOG_WARN("file sql: input changed during scan", K(ret));
    }
  }
  return ret;
}

int FileCsvStream::verify_unchanged() const
{
  return verify_file_snapshot(file_, path_, snapshot_);
}

int FileCsvStream::read_record(std::vector<Field> &fields)
{
  int ret = OB_SUCCESS;
  static const int64_t MAX_RECORD_BYTES = 16LL * 1024 * 1024;
  fields.clear();
  Field field;
  bool in_quotes = false;
  bool after_quote = false;
  bool started = false;
  bool any = false;
  int64_t bytes = 0;
  auto finish_field = [&]() -> int {
    if (fields.size() >= static_cast<size_t>(ObFileScanUtils::OB_FILE_SQL_MAX_COLUMNS)) {
      return OB_SIZE_OVERFLOW;
    }
    field.is_null_ = !field.quoted_ && (nullstr_.empty() ? field.text_.empty() : field.text_ == nullstr_);
    fields.push_back(std::move(field));
    field = Field();
    started = false;
    after_quote = false;
    return OB_SUCCESS;
  };
  while (OB_SUCC(ret)) {
    int ch = fgetc(file_);
    if (EOF == ch) {
      if (ferror(file_)) {
        ret = OB_IO_ERROR;
      } else if (in_quotes) {
        ret = OB_INVALID_ARGUMENT;
      } else if (!any && fields.empty() && field.text_.empty()) {
        ret = OB_ITER_END;
      } else {
        ret = finish_field();
      }
      break;
    }
    if (++bytes > MAX_RECORD_BYTES || ch == '\0') {
      ret = OB_SIZE_OVERFLOW;
      break;
    }
    if (in_quotes) {
      if (ch == escape_) {
        int next = fgetc(file_);
        if (next == EOF) {
          if (escape_ == quote_) {
            in_quotes = false;
            after_quote = true;
          } else {
            ret = ferror(file_) ? OB_IO_ERROR : OB_INVALID_ARGUMENT;
          }
        } else if (escape_ != quote_ || next == quote_) {
          if (++bytes > MAX_RECORD_BYTES || next == '\0') {
            ret = OB_SIZE_OVERFLOW;
          } else {
            field.text_.push_back(static_cast<char>(next));
          }
        } else {
          ungetc(next, file_);
          in_quotes = false;
          after_quote = true;
        }
      } else if (ch == quote_) {
        in_quotes = false;
        after_quote = true;
      } else {
        field.text_.push_back(static_cast<char>(ch));
      }
      any = true;
    } else if (!started && !after_quote && ch == quote_) {
      in_quotes = true;
      field.quoted_ = true;
      started = true;
      any = true;
    } else if (ch == delim_) {
      ret = finish_field();
      any = true;
    } else if (ch == '\n' || ch == '\r') {
      if (ch == '\r') {
        int next = fgetc(file_);
        if (next != '\n' && next != EOF) {
          ungetc(next, file_);
        } else if (next == EOF && ferror(file_)) {
          ret = OB_IO_ERROR;
        }
      }
      if (OB_SUCC(ret) && !any && fields.empty() && field.text_.empty()) {
        // Ignore blank lines without treating a trailing newline as a row.
        bytes = 0;
      } else if (OB_SUCC(ret)) {
        ret = finish_field();
        break;
      }
    } else if (after_quote || ch == quote_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      field.text_.push_back(static_cast<char>(ch));
      started = true;
      any = true;
    }
  }
  return ret;
}

int FileCsvStream::initialize_columns(const std::vector<Field> &first_row, bool header)
{
  int ret = OB_SUCCESS;
  if (first_row.empty() || first_row.size() > static_cast<size_t>(ObFileScanUtils::OB_FILE_SQL_MAX_COLUMNS)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (size_t i = 0; i < first_row.size(); ++i) {
      std::string original = header ? first_row[i].text_ : "";
      if (original.empty()) {
        original = "column" + std::to_string(i);
      }
      std::string candidate = original;
      for (int64_t suffix = 2;; ++suffix) {
        bool used = false;
        for (const Column &column : columns_) {
          if (0 == strcasecmp(column.name_.c_str(), candidate.c_str())) {
            used = true;
            break;
          }
        }
        if (!used) {
          break;
        }
        candidate = original + "__" + std::to_string(suffix);
      }
      Column col;
      col.name_ = std::move(candidate);
      columns_.push_back(std::move(col));
    }
  }
  return ret;
}

int FileCsvStream::infer_row(const std::vector<Field> &fields)
{
  int ret = OB_SUCCESS;
  if (fields.size() != columns_.size()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < fields.size(); ++i) {
      if (fields[i].is_null_) {
        ++columns_[i].null_count_;
      } else {
        int32_t kind = FSQL_KIND_VARCHAR;
        int16_t scale = 0;
        ObString value(static_cast<int32_t>(fields[i].text_.size()), fields[i].text_.data());
        if (OB_FAIL(ObFileScanUtils::detect_value_kind(value, kind, scale))) {
        } else if (OB_FAIL(ObFileScanUtils::merge_kind(columns_[i].kind_, kind, columns_[i].kind_))) {
        } else if (scale > columns_[i].datetime_scale_) {
          columns_[i].datetime_scale_ = scale;
        }
      }
    }
    if (OB_SUCC(ret)) {
      ++row_count_;
    }
  }
  return ret;
}

int FileCsvStream::infer_schema()
{
  int ret = OB_SUCCESS;
  std::vector<Field> fields;
  for (int64_t i = 0; OB_SUCC(ret) && i < skip_; ++i) {
    ret = read_record(fields);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    }
  }
  if (OB_SUCC(ret) && header_) {
    ret = read_record(fields);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    } else if (OB_SUCC(ret)) {
      ret = initialize_columns(fields, true);
    }
  }
  if (OB_SUCC(ret)) {
    data_offset_ = ftello(file_);
    if (data_offset_ < 0) {
      ret = OB_IO_ERROR;
    }
  }
  while (OB_SUCC(ret)) {
    ret = read_record(fields);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    } else if (OB_SUCC(ret) && columns_.empty() && OB_FAIL(initialize_columns(fields, false))) {
    } else if (OB_SUCC(ret) && OB_FAIL(infer_row(fields))) {
    }
  }
  if (OB_SUCC(ret)) {
    for (Column &col : columns_) {
      if (FSQL_KIND_NULL == col.kind_) {
        col.kind_ = FSQL_KIND_VARCHAR;
      }
    }
    ret = rescan();
  }
  return ret;
}

int FileCsvStream::rescan()
{
  int ret = verify_unchanged();
  if (OB_SUCC(ret) && 0 != fseeko(file_, data_offset_, SEEK_SET)) {
    ret = OB_IO_ERROR;
  }
  return ret;
}

int FileCsvStream::next_row(ObIAllocator &alloc, ObIArray<ObObj> &row)
{
  int ret = verify_unchanged();
  row.reset();
  try {
    std::vector<Field> fields;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(read_record(fields))) {
    } else if (fields.size() != columns_.size()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      for (size_t i = 0; OB_SUCC(ret) && i < fields.size(); ++i) {
        ObObj raw;
        ObObj typed;
        if (fields[i].is_null_) {
          raw.set_null();
        } else {
          const size_t size = fields[i].text_.size();
          char *buf = static_cast<char *>(alloc.alloc(size + 1));
          if (nullptr == buf) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, fields[i].text_.data(), size);
            buf[size] = '\0';
            raw.set_varchar(ObString(static_cast<int32_t>(size), buf));
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(ObFileScanUtils::cast_text_cell(alloc, raw, columns_[i].kind_, typed))) {
        } else if (OB_SUCC(ret) && OB_FAIL(row.push_back(typed))) {
        }
      }
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

FileJsonlStream::FileJsonlStream() : file_(nullptr), row_count_(0), snapshot_{} {}
FileJsonlStream::~FileJsonlStream() { close(); }

void FileJsonlStream::close()
{
  if (nullptr != file_) {
    fclose(file_);
    file_ = nullptr;
  }
  path_.clear();
  columns_.clear();
  row_count_ = 0;
}

int FileJsonlStream::init(FILE *file, const ObString &path, const ObFileScanOptions &opts)
{
  int ret = OB_SUCCESS;
  close();
  file_ = file;
  try {
    if (nullptr == file_ || path.empty() || nullptr != memchr(path.ptr(), '\0', path.length()) ||
        opts.sample_size_ != -1 || opts.skip_ != 0 ||
        (!opts.format_.empty() && !ieq(opts.format_, "auto") && !ieq(opts.format_, "newline_delimited"))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      path_.assign(path.ptr(), path.length());
      if (0 != fstat(fileno(file_), &snapshot_) || !S_ISREG(snapshot_.st_mode)) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(verify_unchanged())) {
      } else if (OB_FAIL(infer_schema())) {
      }
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

int FileJsonlStream::verify_unchanged() const
{
  return verify_file_snapshot(file_, path_, snapshot_);
}

int FileJsonlStream::read_line(std::string &line)
{
  static const size_t MAX_RECORD_BYTES = 16U * 1024 * 1024;
  int ret = OB_SUCCESS;
  line.clear();
  while (OB_SUCC(ret)) {
    const int ch = fgetc(file_);
    if (EOF == ch || ch == '\n') {
      if (EOF == ch && ferror(file_)) {
        ret = OB_IO_ERROR;
      } else {
        const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        if (!blank) {
          break;
        } else if (EOF == ch) {
          ret = OB_ITER_END;
        } else {
          line.clear();
        }
      }
    } else if (ch == '\0' || line.size() >= MAX_RECORD_BYTES) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      line.push_back(static_cast<char>(ch));
    }
  }
  return ret;
}

static int json_scalar_kind(const rapidjson::Value &value, int32_t &kind)
{
  int ret = OB_SUCCESS;
  if (value.IsNull()) {
    kind = FSQL_KIND_NULL;
  } else if (value.IsBool()) {
    kind = FSQL_KIND_BOOL;
  } else if (value.IsInt64()) {
    kind = FSQL_KIND_INT;
  } else if (value.IsUint64()) {
    kind = value.GetUint64() <= static_cast<uint64_t>(INT64_MAX) ? FSQL_KIND_INT : FSQL_KIND_DOUBLE;
  } else if (value.IsDouble()) {
    kind = FSQL_KIND_DOUBLE;
  } else if (value.IsString()) {
    kind = FSQL_KIND_VARCHAR;
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int FileJsonlStream::infer_schema()
{
  int ret = OB_SUCCESS;
  std::string line;
  while (OB_SUCC(ret)) {
    ret = read_line(line);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    }
    rapidjson::Document doc;
    if (OB_FAIL(ret)) {
    } else if (doc.Parse(line.data(), line.size()).HasParseError() || !doc.IsObject()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      std::vector<std::string> row_keys;
      for (auto it = doc.MemberBegin(); OB_SUCC(ret) && it != doc.MemberEnd(); ++it) {
        const std::string key(it->name.GetString(), it->name.GetStringLength());
        if (key.empty() || key.find('\0') != std::string::npos) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          for (const std::string &seen : row_keys) {
            if (seen == key) {
              ret = OB_INVALID_ARGUMENT;
              break;
            }
          }
          if (OB_SUCC(ret)) {
            row_keys.push_back(key);
            bool found = false;
            for (const Column &col : columns_) {
              if (col.name_ == key) {
                found = true;
                break;
              }
            }
            if (!found) {
              if (columns_.size() >= static_cast<size_t>(ObFileScanUtils::OB_FILE_SQL_MAX_COLUMNS)) {
                ret = OB_SIZE_OVERFLOW;
              } else {
                Column col;
                col.name_ = key;
                col.null_count_ = row_count_; // absent in every earlier row
                columns_.push_back(std::move(col));
              }
            }
          }
        }
      }
      for (Column &col : columns_) {
        if (OB_FAIL(ret)) {
          break;
        }
        auto it = doc.FindMember(rapidjson::StringRef(col.name_.data(), col.name_.size()));
        int32_t kind = FSQL_KIND_NULL;
        if (it == doc.MemberEnd() || it->value.IsNull()) {
          ++col.null_count_;
        } else if (OB_FAIL(json_scalar_kind(it->value, kind))) {
        } else if (OB_FAIL(ObFileScanUtils::merge_kind(col.kind_, kind, col.kind_))) {
        }
      }
      if (OB_SUCC(ret)) {
        ++row_count_;
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (Column &col : columns_) {
      if (FSQL_KIND_NULL == col.kind_) {
        col.kind_ = FSQL_KIND_VARCHAR;
      }
    }
    ret = rescan();
  }
  return ret;
}

int FileJsonlStream::rescan()
{
  int ret = verify_unchanged();
  if (OB_SUCC(ret) && 0 != fseeko(file_, 0, SEEK_SET)) {
    ret = OB_IO_ERROR;
  }
  return ret;
}

int FileJsonlStream::next_row(ObIAllocator &alloc, ObIArray<ObObj> &row)
{
  int ret = verify_unchanged();
  row.reset();
  try {
    std::string line;
    rapidjson::Document doc;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(read_line(line))) {
    } else if (doc.Parse(line.data(), line.size()).HasParseError() || !doc.IsObject()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      for (const Column &col : columns_) {
        if (OB_FAIL(ret)) {
          break;
        }
        auto it = doc.FindMember(rapidjson::StringRef(col.name_.data(), col.name_.size()));
        ObObj cell;
        if (it == doc.MemberEnd() || it->value.IsNull()) {
          cell.set_null();
        } else {
          const rapidjson::Value &v = it->value;
          if (v.IsBool()) {
            cell.set_tinyint(v.GetBool() ? 1 : 0);
          } else if (v.IsInt64()) {
            cell.set_int(v.GetInt64());
          } else if (v.IsUint64()) {
            const uint64_t u = v.GetUint64();
            if (u <= static_cast<uint64_t>(INT64_MAX)) {
              cell.set_int(static_cast<int64_t>(u));
            } else {
              cell.set_double(static_cast<double>(u));
            }
          } else if (v.IsDouble()) {
            cell.set_double(v.GetDouble());
          } else if (v.IsString()) {
            ObString copy;
            if (OB_FAIL(copy_obstring(alloc, ObString(v.GetStringLength(), v.GetString()), copy))) {
            } else {
              cell.set_varchar(copy);
            }
          } else {
            ret = OB_NOT_SUPPORTED;
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(normalize_json_cell(alloc, col.kind_, cell))) {
        } else if (OB_SUCC(ret) && OB_FAIL(row.push_back(cell))) {
        }
      }
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

struct FileParquetStream::Impl
{
  struct ColumnState {
    ColumnState() : group_(0), group_values_(0), offset_(-1), page_pos_(0),
                    page_alloc_("ParquetPage"), dict_alloc_("ParquetDict") {}
    void reset()
    {
      group_ = 0;
      group_values_ = 0;
      offset_ = -1;
      page_pos_ = 0;
      page_.reset();
      dict_.values_.reset();
      dict_.valid_ = false;
      page_alloc_.reset();
      dict_alloc_.reset();
    }
    int64_t group_;
    int64_t group_values_;
    int64_t offset_;
    int64_t page_pos_;
    ObArenaAllocator page_alloc_;
    ObArenaAllocator dict_alloc_;
    ParquetDict dict_;
    ObSEArray<ObObj, 64> page_;
  };
  Impl() : file_(nullptr), snapshot_{}, metadata_alloc_("ParquetMeta"), rows_(0), emitted_(0) {}
  ~Impl() { if (nullptr != file_) fclose(file_); }
  int read_at(int64_t offset, char *buffer, int64_t length) const;
  int load_footer();
  int decode_page(int64_t column, ColumnState &state);
  int next_value(int64_t column, ObObj &value);
  int rescan();
  FILE *file_;
  std::string path_;
  struct stat snapshot_;
  ObArenaAllocator metadata_alloc_;
  ObSEArray<ParquetSchemaElement, 16> schema_;
  ObSEArray<ParquetColumnChunkMeta, 16> chunks_;
  ObSEArray<int64_t, 16> group_rows_;
  std::vector<FileParquetStream::Column> columns_;
  std::vector<std::unique_ptr<ColumnState>> states_;
  int64_t rows_;
  int64_t emitted_;
};

int FileParquetStream::Impl::read_at(int64_t offset, char *buffer, int64_t length) const
{
  int ret = OB_SUCCESS;
  if (offset < 0 || length < 0 || offset > snapshot_.st_size || length > snapshot_.st_size - offset) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    int64_t done = 0;
    while (OB_SUCC(ret) && done < length) {
      const ssize_t n = pread(fileno(file_), buffer + done, static_cast<size_t>(length - done), offset + done);
      if (n < 0 && EINTR == errno) {
        continue;
      } else if (n <= 0) {
        ret = OB_IO_ERROR;
      } else {
        done += n;
      }
    }
  }
  return ret;
}

int FileParquetStream::Impl::load_footer()
{
  int ret = OB_SUCCESS;
  static const int64_t MAX_FOOTER = 16LL * 1024 * 1024;
  char head[4];
  char tail[8];
  if (snapshot_.st_size < 12 || OB_FAIL(read_at(0, head, sizeof(head))) ||
      OB_FAIL(read_at(snapshot_.st_size - 8, tail, sizeof(tail)))) {
    if (OB_SUCCESS == ret) ret = OB_INVALID_ARGUMENT;
  } else if (0 != MEMCMP(head, "PAR1", 4) || 0 != MEMCMP(tail + 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    uint32_t footer_size = 0;
    for (int i = 0; i < 4; ++i) {
      footer_size |= static_cast<uint32_t>(static_cast<uint8_t>(tail[i])) << (8 * i);
    }
    if (footer_size == 0 || footer_size > MAX_FOOTER || footer_size > snapshot_.st_size - 12) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      std::vector<char> bytes(footer_size);
      if (OB_FAIL(read_at(snapshot_.st_size - 8 - footer_size, bytes.data(), footer_size))) {
      } else {
        ThriftReader reader(bytes.data(), footer_size);
        int16_t fid = 0, last = 0;
        int8_t type = 0;
        bool stop = false;
        while (OB_SUCC(ret) && !stop) {
          if (OB_FAIL(reader.read_field_begin(type, fid, stop, last))) {
          } else if (stop) {
            break;
          } else if (2 == fid && 9 == type) {
            int8_t element = 0;
            int64_t count = 0;
            if (OB_FAIL(reader.read_list_begin(element, count))) {
            } else if (count <= 0 || count > ObFileScanUtils::OB_FILE_SQL_MAX_COLUMNS + 1) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
                ParquetSchemaElement entry;
                if (OB_FAIL(parse_parquet_schema_element(reader, metadata_alloc_, entry))) {
                } else if (OB_FAIL(schema_.push_back(entry))) {
                }
              }
            }
          } else if (4 == fid && 9 == type) {
            int8_t element = 0;
            int64_t count = 0;
            if (OB_FAIL(reader.read_list_begin(element, count))) {
            } else if (count < 0 || count > 100000) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              for (int64_t group = 0; OB_SUCC(ret) && group < count; ++group) {
                int16_t id = 0, previous = 0;
                int8_t field_type = 0;
                bool done = false;
                int64_t group_row_count = -1;
                while (OB_SUCC(ret) && !done) {
                  if (OB_FAIL(reader.read_field_begin(field_type, id, done, previous))) {
                  } else if (done) {
                    break;
                  } else if (1 == id && 9 == field_type) {
                    int8_t chunk_type = 0;
                    int64_t chunk_count = 0;
                    if (OB_FAIL(reader.read_list_begin(chunk_type, chunk_count))) {
                    } else if (chunk_count < 0 || chunk_count > ObFileScanUtils::OB_FILE_SQL_MAX_COLUMNS) {
                      ret = OB_INVALID_ARGUMENT;
                    } else {
                      for (int64_t i = 0; OB_SUCC(ret) && i < chunk_count; ++i) {
                        ParquetColumnChunkMeta chunk;
                        if (OB_FAIL(parse_parquet_column_meta(reader, metadata_alloc_, chunk))) {
                        } else if (OB_FAIL(chunks_.push_back(chunk))) {
                        }
                      }
                    }
                  } else if (3 == id) {
                    ret = reader.read_i64(group_row_count);
                  } else {
                    ret = reader.skip_value(field_type);
                  }
                }
                if (OB_SUCC(ret) && group_row_count < 0) {
                  ret = OB_INVALID_ARGUMENT;
                } else if (OB_SUCC(ret) && OB_FAIL(group_rows_.push_back(group_row_count))) {
                }
              }
            }
          } else {
            ret = reader.skip_value(type);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (schema_.count() < 2 || schema_.at(0).num_children_ != schema_.count() - 1 ||
        chunks_.count() != group_rows_.count() * (schema_.count() - 1)) {
      ret = OB_NOT_SUPPORTED;
    } else {
      for (int64_t group = 0; OB_SUCC(ret) && group < group_rows_.count(); ++group) {
        if (group_rows_.at(group) > INT64_MAX - rows_) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          rows_ += group_rows_.at(group);
        }
      }
      for (int64_t c = 1; OB_SUCC(ret) && c < schema_.count(); ++c) {
        const ParquetSchemaElement &entry = schema_.at(c);
        if (entry.type_ < 0 || entry.num_children_ != 0 ||
            (entry.repetition_type_ != 0 && entry.repetition_type_ != 1)) {
          ret = OB_NOT_SUPPORTED;
        } else {
          Column col;
          col.name_.assign(entry.name_.ptr(), entry.name_.length());
          col.kind_ = static_cast<int32_t>(extract_parquet_kind(entry, nullptr));
          col.datetime_scale_ = (col.kind_ == FSQL_KIND_DATETIME || col.kind_ == FSQL_KIND_TIME) ? 6 : 0;
          columns_.push_back(std::move(col));
          states_.emplace_back(new ColumnState());
          for (int64_t g = 0; OB_SUCC(ret) && g < group_rows_.count(); ++g) {
            const ParquetColumnChunkMeta &chunk = chunks_.at(g * (schema_.count() - 1) + c - 1);
            if (chunk.column_name_ != entry.name_ || chunk.num_values_ != group_rows_.at(g)) {
              ret = OB_INVALID_ARGUMENT;
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = verify_file_snapshot(file_, path_, snapshot_);
  }
  return ret;
}

int FileParquetStream::Impl::decode_page(int64_t column, ColumnState &state)
{
  static const int64_t MAX_PAGE_BYTES = 16LL * 1024 * 1024;
  static const int64_t MAX_PAGE_ROWS = 1000000;
  int ret = OB_SUCCESS;
  const int64_t col_count = columns_.size();
  const ParquetSchemaElement &schema = schema_.at(column + 1);
  const ParquetColumnChunkMeta &chunk = chunks_.at(state.group_ * col_count + column);
  if (state.offset_ < 0) {
    state.offset_ = chunk.dictionary_page_offset_ >= 0 && chunk.dictionary_page_offset_ < chunk.data_page_offset_
        ? chunk.dictionary_page_offset_ : chunk.data_page_offset_;
  }
  if (state.offset_ < 0 || state.offset_ >= snapshot_.st_size) {
    return OB_INVALID_ARGUMENT;
  }
  const int64_t header_capacity = std::min<int64_t>(65536, snapshot_.st_size - state.offset_);
  std::vector<char> header(header_capacity);
  ParquetPageHeader page;
  if (OB_FAIL(read_at(state.offset_, header.data(), header_capacity))) {
    return ret;
  }
  ThriftReader reader(header.data(), header_capacity);
  if (OB_FAIL(parse_parquet_page_header(reader, page))) {
    return ret;
  }
  const int64_t payload_offset = state.offset_ + reader.pos();
  if (page.compressed_size_ < 0 || page.compressed_size_ > MAX_PAGE_BYTES + 1024 * 1024 ||
      page.uncompressed_size_ < 0 || page.uncompressed_size_ > MAX_PAGE_BYTES ||
      page.num_values_ < 0 || page.num_values_ > MAX_PAGE_ROWS ||
      payload_offset > snapshot_.st_size || page.compressed_size_ > snapshot_.st_size - payload_offset) {
    return OB_INVALID_ARGUMENT;
  }
  std::vector<char> payload(page.compressed_size_);
  if (OB_FAIL(read_at(payload_offset, payload.data(), page.compressed_size_))) {
    return ret;
  }
  state.offset_ = payload_offset + page.compressed_size_;
  if (2 == page.type_) {
    if (state.group_values_ != 0 || state.dict_.valid_) {
      return OB_INVALID_ARGUMENT;
    }
    state.dict_.values_.reset();
    state.dict_alloc_.reuse();
    char *plain = static_cast<char *>(state.dict_alloc_.alloc(page.uncompressed_size_ + 1));
    int64_t outlen = 0;
    if (nullptr == plain) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(parquet_decompress(chunk.codec_, payload.data(), page.compressed_size_,
                                           plain, page.uncompressed_size_, outlen))) {
    } else if (outlen != page.uncompressed_size_) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(decode_plain_values(state.dict_alloc_, plain, outlen, schema,
                                            schema.type_, page.num_values_, state.dict_.values_))) {
    } else {
      state.dict_.valid_ = true;
    }
    return ret;
  } else if (0 != page.type_ && 3 != page.type_) {
    return OB_SUCCESS; // optional index page
  } else if (page.num_values_ > group_rows_.at(state.group_) - state.group_values_) {
    return OB_INVALID_ARGUMENT;
  }

  state.page_.reset();
  state.page_alloc_.reuse();
  state.page_pos_ = 0;
  char *plain = static_cast<char *>(state.page_alloc_.alloc(page.uncompressed_size_ + 1));
  int64_t outlen = 0;
  const char *def_data = nullptr;
  int64_t def_len = 0;
  const char *values_data = nullptr;
  const int64_t max_def = (1 == schema.repetition_type_) ? 1 : 0;
  if (nullptr == plain) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (0 == page.type_) {
    if (OB_FAIL(parquet_decompress(chunk.codec_, payload.data(), page.compressed_size_,
                                    plain, page.uncompressed_size_, outlen))) {
    } else if (outlen != page.uncompressed_size_) {
      ret = OB_INVALID_ARGUMENT;
    } else if (max_def > 0) {
      if (outlen < 4) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        uint32_t size = 0;
        for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(static_cast<uint8_t>(plain[i])) << (8 * i);
        if (size > static_cast<uint64_t>(outlen - 4)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          def_data = plain + 4;
          def_len = size;
          values_data = plain + 4 + size;
          outlen -= 4 + size;
        }
      }
    } else {
      values_data = plain;
    }
  } else if (page.v2_rep_len_ != 0 || page.v2_def_len_ < 0 ||
             page.v2_def_len_ > page.compressed_size_ || page.v2_def_len_ > page.uncompressed_size_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    def_data = payload.data();
    def_len = page.v2_def_len_;
    const int64_t src_len = page.compressed_size_ - def_len;
    if (OB_FAIL(parquet_decompress(page.v2_is_compressed_ ? chunk.codec_ : 0,
                                    payload.data() + def_len, src_len, plain,
                                    page.uncompressed_size_ - def_len, outlen))) {
    } else if (outlen != page.uncompressed_size_ - def_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      values_data = plain;
    }
  }
  std::vector<uint8_t> present(page.num_values_, 1);
  int64_t non_null = page.num_values_;
  if (OB_SUCC(ret) && max_def > 0) {
    if (nullptr == def_data || def_len <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      RleDecoder levels(def_data, def_len, 1);
      non_null = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < page.num_values_; ++i) {
        int64_t level = 0;
        if (OB_FAIL(levels.next(level))) {
          if (OB_ITER_END == ret) ret = OB_INVALID_ARGUMENT;
        } else if (level != 0 && level != 1) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          present[i] = static_cast<uint8_t>(level);
          non_null += level;
        }
      }
    }
  }
  ObSEArray<ObObj, 64> decoded;
  if (OB_FAIL(ret)) {
  } else if (0 == page.encoding_) {
    ret = decode_plain_values(state.page_alloc_, values_data, outlen, schema,
                              schema.type_, non_null, decoded);
  } else if (2 == page.encoding_ || 8 == page.encoding_) {
    if (!state.dict_.valid_ || outlen < 1) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const int32_t bit_width = static_cast<uint8_t>(values_data[0]);
      if (bit_width > 32) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        RleDecoder indexes(values_data + 1, outlen - 1, bit_width);
        for (int64_t i = 0; OB_SUCC(ret) && i < non_null; ++i) {
          int64_t index = 0;
          if (OB_FAIL(indexes.next(index))) {
            if (OB_ITER_END == ret) ret = OB_INVALID_ARGUMENT;
          } else if (index < 0 || index >= state.dict_.values_.count()) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(decoded.push_back(state.dict_.values_.at(index)))) {
          }
        }
      }
    }
  } else if (3 == page.encoding_ && 0 == schema.type_) {
    if (outlen < 4) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      uint32_t size = 0;
      for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(static_cast<uint8_t>(values_data[i])) << (8 * i);
      if (size != static_cast<uint64_t>(outlen - 4)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        RleDecoder bools(values_data + 4, size, 1);
        for (int64_t i = 0; OB_SUCC(ret) && i < non_null; ++i) {
          int64_t value = 0;
          ObObj cell;
          if (OB_FAIL(bools.next(value))) {
            if (OB_ITER_END == ret) ret = OB_INVALID_ARGUMENT;
          } else {
            cell.set_tinyint(value ? 1 : 0);
            if (OB_FAIL(decoded.push_back(cell))) {
            }
          }
        }
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  int64_t value_index = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < page.num_values_; ++i) {
    ObObj cell;
    if (!present[i]) {
      cell.set_null();
    } else if (value_index >= decoded.count()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      cell = decoded.at(value_index++);
    }
    if (OB_SUCC(ret) && OB_FAIL(state.page_.push_back(cell))) {
    }
  }
  if (OB_SUCC(ret)) {
    state.group_values_ += page.num_values_;
  }
  return ret;
}

int FileParquetStream::Impl::next_value(int64_t column, ObObj &value)
{
  int ret = OB_SUCCESS;
  ColumnState &state = *states_[column];
  while (OB_SUCC(ret)) {
    if (state.page_pos_ < state.page_.count()) {
      value = state.page_.at(state.page_pos_++);
      break;
    } else if (state.group_ >= group_rows_.count()) {
      ret = OB_ITER_END;
    } else if (state.group_values_ == group_rows_.at(state.group_)) {
      ++state.group_;
      state.group_values_ = 0;
      state.offset_ = -1;
      state.page_.reset();
      state.page_pos_ = 0;
      state.page_alloc_.reuse();
      state.dict_.values_.reset();
      state.dict_.valid_ = false;
      state.dict_alloc_.reuse();
    } else if (OB_FAIL(decode_page(column, state))) {
    }
  }
  return ret;
}

int FileParquetStream::Impl::rescan()
{
  int ret = verify_file_snapshot(file_, path_, snapshot_);
  if (OB_SUCC(ret)) {
    for (const auto &state : states_) state->reset();
    emitted_ = 0;
  }
  return ret;
}

FileParquetStream::FileParquetStream() : impl_(nullptr) {}
FileParquetStream::~FileParquetStream() { close(); }
void FileParquetStream::close() { delete impl_; impl_ = nullptr; }

int FileParquetStream::init(FILE *file, const ObString &path)
{
  int ret = OB_SUCCESS;
  close();
  try {
    impl_ = new Impl();
    impl_->file_ = file;
    file = nullptr;
    if (nullptr == impl_->file_ || path.empty() || nullptr != memchr(path.ptr(), '\0', path.length())) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      impl_->path_.assign(path.ptr(), path.length());
      if (0 != fstat(fileno(impl_->file_), &impl_->snapshot_) || !S_ISREG(impl_->snapshot_.st_mode)) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(verify_file_snapshot(impl_->file_, impl_->path_, impl_->snapshot_))) {
      } else if (OB_FAIL(impl_->load_footer())) {
      }
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (nullptr != file) fclose(file);
  if (OB_FAIL(ret)) close();
  return ret;
}

int FileParquetStream::rescan()
{
  return nullptr == impl_ ? OB_NOT_INIT : impl_->rescan();
}

int FileParquetStream::next_row(ObIAllocator &alloc, ObIArray<ObObj> &row)
{
  int ret = OB_SUCCESS;
  row.reset();
  try {
    if (nullptr == impl_) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(verify_file_snapshot(impl_->file_, impl_->path_, impl_->snapshot_))) {
    } else if (impl_->emitted_ >= impl_->rows_) {
      ret = OB_ITER_END;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(impl_->columns_.size()); ++i) {
        ObObj cell;
        if (OB_FAIL(impl_->next_value(i, cell))) {
          if (OB_ITER_END == ret) ret = OB_INVALID_ARGUMENT;
        } else if (cell.is_string_type()) {
          ObString copied;
          if (OB_FAIL(copy_obstring(alloc, cell.get_string(), copied))) {
          } else {
            cell.set_varchar(copied);
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(row.push_back(cell))) {
        }
      }
      if (OB_SUCC(ret)) ++impl_->emitted_;
    }
  } catch (const std::bad_alloc &) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

const std::vector<FileParquetStream::Column> &FileParquetStream::columns() const
{
  static const std::vector<Column> empty;
  return nullptr == impl_ ? empty : impl_->columns_;
}
int64_t FileParquetStream::row_count() const { return nullptr == impl_ ? 0 : impl_->rows_; }
const struct stat &FileParquetStream::snapshot() const
{
  static const struct stat empty{};
  return nullptr == impl_ ? empty : impl_->snapshot_;
}

} // end namespace sql
} // end namespace oceanbase
