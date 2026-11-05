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

#pragma once

#include "lib/ob_define.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_object.h"
#include "common/object/ob_obj_type.h"
#include <cstdio>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace oceanbase
{
namespace sql
{
// Deterministic scan utilities for the `files as database` feature:
// read_csv / read_json / read_ndjson / read_parquet table functions and glob().
//
// Everything here is a pure function of (file content, options): the same
// input always produces the same inferred schema, so type inference is
// deterministic. The whole file is scanned (DuckDB sample_size = -1
// semantics); partial sampling is intentionally not supported.

// Logical kinds used during type inference. The merge lattice follows the
// DuckDB CSV sniffer candidate order:
//   NULL < BOOLEAN < BIGINT < DOUBLE < TIME < DATE < DATETIME < VARCHAR
// merge rules: INT+DOUBLE->DOUBLE, DATE+DATETIME->DATETIME, anything
// incompatible -> VARCHAR.
enum ObFileSqlTypeKind
{
  FSQL_KIND_NULL = 0,
  FSQL_KIND_BOOL,
  FSQL_KIND_INT,
  FSQL_KIND_DOUBLE,
  FSQL_KIND_TIME,
  FSQL_KIND_DATE,
  FSQL_KIND_DATETIME,
  FSQL_KIND_VARCHAR,
};

struct ObFileSqlColumn
{
  ObFileSqlColumn() : kind_(FSQL_KIND_VARCHAR), has_null_(false), null_count_(0),
                      datetime_scale_(0) {}
  common::ObString name_;
  int32_t kind_;
  bool has_null_;
  int64_t null_count_;
  // for DATETIME columns: max fractional-second digits observed (0..6)
  int16_t datetime_scale_;
};

// Scan options shared by read_csv/read_json/read_ndjson/read_parquet.
// Defaults follow DuckDB: header = true, delim ",", quote '"', escape '"',
// empty unquoted field = NULL, sample_size = -1 (whole file).
struct ObFileScanOptions
{
  ObFileScanOptions()
    : header_(true), sample_size_(-1), skip_(0),
      delim_(1, ","), quote_(1, "\""), escape_(1, "\""), nullstr_(0, ""),
      format_(0, "") {}
  bool header_;
  int64_t sample_size_;
  int64_t skip_;
  common::ObString delim_;
  common::ObString quote_;
  common::ObString escape_;
  common::ObString nullstr_;
  common::ObString format_; // json: array|newline_delimited|auto; csv: csv|auto
};

// Scan result: column metadata plus a row-major ObObj matrix. Non-null cells
// hold either raw VARCHAR payloads (csv/json text forms, cast later with the
// final column kind) or already typed values (parquet).
struct ObFileScanResult
{
  ObFileScanResult() : row_count_(0) {}
  int64_t column_count() const { return columns_.count(); }
  common::ObSEArray<ObFileSqlColumn, 16> columns_;
  common::ObSEArray<common::ObObj, 64> cells_; // row-major, rows * columns
  int64_t row_count_;
};

class ObFileScanUtils
{
public:
  // ---------- file access ----------
  // Opens a regular file beneath secure_file_priv, rejecting symlink escapes
  // and walking the resolved path relative to directory descriptors.  On
  // success the caller owns file and canonical_path belongs to alloc.
  static int open_secure_readonly(common::ObIAllocator &alloc,
                                  const common::ObString &path,
                                  const common::ObString &secure_file_priv,
                                  FILE *&file,
                                  common::ObString &canonical_path);
  static int file_identity(const struct stat &snapshot, common::ObIArray<int64_t> &identity);
  static int glob_secure(common::ObIAllocator &alloc, const common::ObString &pattern,
                         const common::ObString &secure_file_priv,
                         common::ObIArray<common::ObString> &files);
  // Reads the whole file into memory. Fails on non-regular files, missing
  // files, embedded '\0' and files larger than OB_FILE_SQL_MAX_FILE_SIZE.
  static int read_whole_file(common::ObIAllocator &alloc,
                             const common::ObString &path,
                             common::ObString &content);
  // Expands a glob pattern (POSIX glob(3) semantics, sorted output).
  static int glob_files(common::ObIAllocator &alloc,
                        const common::ObString &pattern,
                        common::ObIArray<common::ObString> &files);

  // ---------- format scanners ----------
  static int scan_csv(common::ObIAllocator &alloc,
                      const common::ObString &path,
                      const ObFileScanOptions &opts,
                      ObFileScanResult &result);
  static int scan_json(common::ObIAllocator &alloc,
                       const common::ObString &path,
                       const ObFileScanOptions &opts,
                       ObFileScanResult &result); // read_json, format option honored
  static int scan_ndjson(common::ObIAllocator &alloc,
                         const common::ObString &path,
                         const ObFileScanOptions &opts,
                         ObFileScanResult &result); // read_ndjson shorthand
  static int scan_parquet(common::ObIAllocator &alloc,
                          const common::ObString &path,
                          ObFileScanResult &result);

  // ---------- type helpers ----------
  // Deterministic single-value kind detection (candidate order above).
  static int detect_value_kind(const common::ObString &text, int32_t &kind, int16_t &dt_scale);
  // Deterministic pairwise merge.
  static int merge_kind(int32_t a, int32_t b, int32_t &merged);
  // Kind -> display name used by DESCRIBE (BOOLEAN/BIGINT/DOUBLE/TIME/DATE/DATETIME/VARCHAR)
  static const char *kind_type_name(int32_t kind);
  // Kind -> OceanBase object type
  static common::ObObjType kind_obj_type(int32_t kind);
  // Convert a raw VARCHAR cell into the typed cell required by the column
  // kind. Returns OB_SUCCESS + null obj for NULL cells.
  static int cast_text_cell(common::ObIAllocator &alloc,
                            const common::ObObj &cell,
                            int32_t kind,
                            common::ObObj &out);
  // CSV output helper shared by COPY: appends `text` to buf, quoting it when
  // it contains delim / quote / CR / LF. Returns the appended string.
  static int csv_quote_field(common::ObIAllocator &alloc,
                             const common::ObString &text,
                             const common::ObString &delim,
                             const common::ObString &quote,
                             common::ObString &out);

  static const int64_t OB_FILE_SQL_MAX_FILE_SIZE = 512LL * 1024 * 1024; // 512MB
  static const int64_t OB_FILE_SQL_MAX_COLUMNS = 4096;
  static const int64_t OB_FILE_SQL_MAX_ROWS = 4LL * 1024 * 1024;

private:
  static int parse_csv_content(common::ObIAllocator &alloc,
                               const common::ObString &content,
                               const ObFileScanOptions &opts,
                               ObFileScanResult &result,
                               bool ndjson_like_name_fallback);
  static int parse_json_content(common::ObIAllocator &alloc,
                                const common::ObString &content,
                                const common::ObString &explicit_format,
                                ObFileScanResult &result);
  static int infer_columns_from_rows(common::ObIAllocator &alloc,
                                     common::ObIArray<common::ObSEArray<common::ObObj, 16>> &rows,
                                     bool has_header_row,
                                     ObFileScanResult &result);
  DISALLOW_COPY_AND_ASSIGN(ObFileScanUtils);
};

// Bounded-memory CSV reader.  Its caller must first authorize the resolved
// path against secure_file_priv and open the file without following an
// untrusted path; this class does not grant filesystem access.  init() takes
// ownership of the supplied stream, including on failure.
class FileCsvStream
{
public:
  struct Column {
    std::string name_;
    int32_t kind_ = FSQL_KIND_NULL;
    int16_t datetime_scale_ = 0;
    int64_t null_count_ = 0;
  };
  FileCsvStream();
  ~FileCsvStream();
  int init(FILE *file, const common::ObString &path, const ObFileScanOptions &opts);
  int rescan();
  int next_row(common::ObIAllocator &alloc, common::ObIArray<common::ObObj> &row);
  const std::vector<Column> &columns() const { return columns_; }
  int64_t row_count() const { return row_count_; }
  const struct stat &snapshot() const { return snapshot_; }
  void close();
private:
  struct Field {
    std::string text_;
    bool is_null_ = false;
    bool quoted_ = false;
  };
  int read_record(std::vector<Field> &fields);
  int infer_schema();
  int verify_unchanged() const;
  int initialize_columns(const std::vector<Field> &first_row, bool header);
  int infer_row(const std::vector<Field> &fields);
  FILE *file_;
  std::string path_;
  std::string nullstr_;
  char delim_;
  char quote_;
  char escape_;
  bool header_;
  int64_t skip_;
  int64_t row_count_;
  int64_t data_offset_;
  struct stat snapshot_;
  std::vector<Column> columns_;
  DISALLOW_COPY_AND_ASSIGN(FileCsvStream);
};

// Like FileCsvStream, this consumes an already-authorized FILE and keeps only
// the union schema plus one JSONL record in memory.
class FileJsonlStream
{
public:
  using Column = FileCsvStream::Column;
  FileJsonlStream();
  ~FileJsonlStream();
  int init(FILE *file, const common::ObString &path, const ObFileScanOptions &opts);
  int rescan();
  int next_row(common::ObIAllocator &alloc, common::ObIArray<common::ObObj> &row);
  const std::vector<Column> &columns() const { return columns_; }
  int64_t row_count() const { return row_count_; }
  const struct stat &snapshot() const { return snapshot_; }
  void close();
private:
  int read_line(std::string &line);
  int infer_schema();
  int verify_unchanged() const;
  FILE *file_;
  std::string path_;
  int64_t row_count_;
  struct stat snapshot_;
  std::vector<Column> columns_;
  DISALLOW_COPY_AND_ASSIGN(FileJsonlStream);
};

// Flat Parquet reader: footer metadata is retained, data is decoded a page
// at a time, and the input stream must already be authorized by the caller.
class FileParquetStream
{
public:
  using Column = FileCsvStream::Column;
  FileParquetStream();
  ~FileParquetStream();
  int init(FILE *file, const common::ObString &path);
  int rescan();
  int next_row(common::ObIAllocator &alloc, common::ObIArray<common::ObObj> &row);
  const std::vector<Column> &columns() const;
  int64_t row_count() const;
  const struct stat &snapshot() const;
  void close();
private:
  struct Impl;
  Impl *impl_;
  DISALLOW_COPY_AND_ASSIGN(FileParquetStream);
};

} // end namespace sql
} // end namespace oceanbase

