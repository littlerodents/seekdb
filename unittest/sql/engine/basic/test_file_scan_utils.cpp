/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include "lib/allocator/page_arena.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
class TempFile
{
public:
  explicit TempFile(const char *content)
  {
    char path[] = "/tmp/seekdb-file-sql-test-XXXXXX";
    const int fd = mkstemp(path);
    if (fd >= 0) {
      path_ = path;
      FILE *file = fdopen(fd, "wb");
      if (file) {
        fwrite(content, 1, strlen(content), file);
        fclose(file);
      } else {
        ::close(fd);
      }
    }
  }
  ~TempFile() { if (!path_.empty()) unlink(path_.c_str()); }
  FILE *open() const { return fopen(path_.c_str(), "rb"); }
  const std::string &path() const { return path_; }
private:
  std::string path_;
};

TEST(FileSqlTypeInference, HandlesBoundariesAndInvalidDates)
{
  struct Case { const char *value_; int32_t expected_; };
  const Case cases[] = {{"-9223372036854775808", FSQL_KIND_INT},
                        {"9223372036854775807", FSQL_KIND_INT},
                        {"9223372036854775808", FSQL_KIND_DOUBLE},
                        {"2000-02-29", FSQL_KIND_DATE},
                        {"1900-02-29", FSQL_KIND_VARCHAR},
                        {"2026-02-30", FSQL_KIND_VARCHAR},
                        {"99:99:99", FSQL_KIND_VARCHAR},
                        {"", FSQL_KIND_VARCHAR}};
  for (const Case &c : cases) {
    int32_t actual = -1;
    int16_t scale = 0;
    ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::detect_value_kind(ObString(c.value_), actual, scale));
    EXPECT_EQ(c.expected_, actual) << c.value_;
  }
}

TEST(FileSqlCsvStream, ReadsQuotedRecordsAndRescans)
{
  TempFile file("id,id,note\r\n1,4,\"hello,\nworld\"\r\n2,,\"\"\r\n");
  ASSERT_FALSE(file.path().empty());
  FileCsvStream reader;
  ObFileScanOptions opts;
  ASSERT_EQ(OB_SUCCESS, reader.init(file.open(), ObString(file.path().c_str()), opts));
  ASSERT_EQ(3U, reader.columns().size());
  EXPECT_EQ("id__2", reader.columns()[1].name_);
  EXPECT_EQ(2, reader.row_count());
  ObArenaAllocator alloc;
  ObSEArray<ObObj, 8> row;
  ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
  ASSERT_EQ(3, row.count());
  EXPECT_EQ(1, row.at(0).get_int());
  EXPECT_EQ(ObString("hello,\nworld"), row.at(2).get_string());
  alloc.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
  EXPECT_TRUE(row.at(1).is_null());
  EXPECT_FALSE(row.at(2).is_null());
  EXPECT_EQ(0, row.at(2).get_string().length());
  EXPECT_EQ(OB_ITER_END, reader.next_row(alloc, row));
  ASSERT_EQ(OB_SUCCESS, reader.rescan());
  alloc.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
  EXPECT_EQ(1, row.at(0).get_int());
}

TEST(FileSqlCsvStream, RejectsMalformedAndChangedFiles)
{
  ObFileScanOptions opts;
  TempFile bad("id,name\n1,\"bad\"tail\n");
  FileCsvStream invalid;
  EXPECT_NE(OB_SUCCESS, invalid.init(bad.open(), ObString(bad.path().c_str()), opts));

  TempFile changed("id,name\n1,one\n");
  FileCsvStream reader;
  ASSERT_EQ(OB_SUCCESS, reader.init(changed.open(), ObString(changed.path().c_str()), opts));
  FILE *out = fopen(changed.path().c_str(), "ab");
  ASSERT_NE(nullptr, out);
  ASSERT_EQ(6U, fwrite("2,two\n", 1, 6, out));
  ASSERT_EQ(0, fclose(out));
  ObArenaAllocator alloc;
  ObSEArray<ObObj, 8> row;
  EXPECT_NE(OB_SUCCESS, reader.next_row(alloc, row));
  EXPECT_NE(OB_SUCCESS, reader.rescan());
}

TEST(FileSqlSecurity, RestrictsReadsToTheConfiguredDirectory)
{
  char parent[] = "/tmp/seekdb-file-sql-dir-XXXXXX";
  ASSERT_NE(nullptr, mkdtemp(parent));
  const std::string root = std::string(parent) + "/allowed";
  const std::string sibling = std::string(parent) + "/allowed-extra";
  ASSERT_EQ(0, mkdir(root.c_str(), 0700));
  ASSERT_EQ(0, mkdir(sibling.c_str(), 0700));
  const std::string good = root + "/ok.csv";
  const std::string outside = sibling + "/outside.csv";
  FILE *output = fopen(good.c_str(), "wb");
  ASSERT_NE(nullptr, output);
  fputs("id\n1\n", output);
  fclose(output);
  output = fopen(outside.c_str(), "wb");
  ASSERT_NE(nullptr, output);
  fputs("secret\n", output);
  fclose(output);
  const std::string link = root + "/escape.csv";
  ASSERT_EQ(0, symlink(outside.c_str(), link.c_str()));

  ObArenaAllocator alloc;
  ObString canonical;
  FILE *file = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::open_secure_readonly(
      alloc, ObString(good.c_str()), ObString(root.c_str()), file, canonical));
  ASSERT_NE(nullptr, file);
  FileCsvStream reader;
  ObFileScanOptions opts;
  ASSERT_EQ(OB_SUCCESS, reader.init(file, canonical, opts));
  EXPECT_EQ(1, reader.row_count());
  file = nullptr;
  EXPECT_NE(OB_SUCCESS, ObFileScanUtils::open_secure_readonly(
      alloc, ObString(outside.c_str()), ObString(root.c_str()), file, canonical));
  EXPECT_EQ(nullptr, file);
  EXPECT_NE(OB_SUCCESS, ObFileScanUtils::open_secure_readonly(
      alloc, ObString(link.c_str()), ObString(root.c_str()), file, canonical));
  EXPECT_EQ(nullptr, file);
  EXPECT_NE(OB_SUCCESS, ObFileScanUtils::open_secure_readonly(
      alloc, ObString(good.c_str()), ObString(), file, canonical));
  EXPECT_EQ(nullptr, file);
  reader.close();
  EXPECT_EQ(0, unlink(link.c_str()));
  EXPECT_EQ(0, unlink(good.c_str()));
  EXPECT_EQ(0, unlink(outside.c_str()));
  EXPECT_EQ(0, rmdir(root.c_str()));
  EXPECT_EQ(0, rmdir(sibling.c_str()));
  EXPECT_EQ(0, rmdir(parent));
}

TEST(FileSqlSecurity, GlobDoesNotTraverseOutsideSymlinks)
{
  char parent[] = "/tmp/seekdb-glob-test-XXXXXX";
  ASSERT_NE(nullptr, mkdtemp(parent));
  const std::string root = std::string(parent) + "/allowed";
  const std::string sub = root + "/inside";
  const std::string outside = std::string(parent) + "/outside";
  ASSERT_EQ(0, mkdir(root.c_str(), 0700));
  ASSERT_EQ(0, mkdir(sub.c_str(), 0700));
  ASSERT_EQ(0, mkdir(outside.c_str(), 0700));
  const std::string good = sub + "/safe.csv";
  const std::string secret = outside + "/secret.csv";
  FILE *file = fopen(good.c_str(), "wb"); ASSERT_NE(nullptr, file); fclose(file);
  file = fopen(secret.c_str(), "wb"); ASSERT_NE(nullptr, file); fclose(file);
  const std::string escape = root + "/escape";
  ASSERT_EQ(0, symlink(outside.c_str(), escape.c_str()));
  ObArenaAllocator alloc;
  ObSEArray<ObString, 8> files;
  const std::string pattern = root + "/*/*.csv";
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::glob_secure(alloc, ObString(pattern.c_str()),
                                                      ObString(root.c_str()), files));
  ASSERT_EQ(1, files.count());
  EXPECT_TRUE(files.at(0).suffix_match("/inside/safe.csv"));
  const std::string denied = outside + "/*.csv";
  EXPECT_NE(OB_SUCCESS, ObFileScanUtils::glob_secure(alloc, ObString(denied.c_str()),
                                                     ObString(root.c_str()), files));
  EXPECT_EQ(0, unlink(escape.c_str()));
  EXPECT_EQ(0, unlink(good.c_str()));
  EXPECT_EQ(0, unlink(secret.c_str()));
  EXPECT_EQ(0, rmdir(sub.c_str()));
  EXPECT_EQ(0, rmdir(root.c_str()));
  EXPECT_EQ(0, rmdir(outside.c_str()));
  EXPECT_EQ(0, rmdir(parent));
}

TEST(FileSqlJson, ConvertsValuesWhenUnionTypeWidens)
{
  TempFile file("{\"value\":1}\n{\"value\":\"two\"}\n");
  ObArenaAllocator alloc;
  ObFileScanOptions opts;
  ObFileScanResult result;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::scan_ndjson(alloc, ObString(file.path().c_str()), opts, result));
  ASSERT_EQ(1, result.column_count());
  ASSERT_EQ(2, result.row_count_);
  EXPECT_EQ(FSQL_KIND_VARCHAR, result.columns_.at(0).kind_);
  ASSERT_TRUE(result.cells_.at(0).is_string_type());
  EXPECT_EQ(ObString("1"), result.cells_.at(0).get_string());
  EXPECT_EQ(ObString("two"), result.cells_.at(1).get_string());
}

TEST(FileSqlJsonlStream, UnionsFieldsAndRescansWithoutMaterializingRows)
{
  TempFile file("{\"id\":1,\"value\":1}\n{\"id\":2,\"value\":\"two\",\"name\":\"x\"}\n{\"id\":3}\n");
  FileJsonlStream reader;
  ObFileScanOptions opts;
  ASSERT_EQ(OB_SUCCESS, reader.init(file.open(), ObString(file.path().c_str()), opts));
  ASSERT_EQ(3U, reader.columns().size());
  EXPECT_EQ(3, reader.row_count());
  EXPECT_EQ(FSQL_KIND_VARCHAR, reader.columns()[1].kind_);
  EXPECT_EQ(2, reader.columns()[2].null_count_);
  ObArenaAllocator alloc;
  ObSEArray<ObObj, 8> row;
  ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
  EXPECT_EQ(ObString("1"), row.at(1).get_string());
  EXPECT_TRUE(row.at(2).is_null());
  alloc.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
  EXPECT_EQ(ObString("two"), row.at(1).get_string());
  EXPECT_EQ(ObString("x"), row.at(2).get_string());
  alloc.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
  EXPECT_TRUE(row.at(1).is_null());
  EXPECT_TRUE(row.at(2).is_null());
  EXPECT_EQ(OB_ITER_END, reader.next_row(alloc, row));
  ASSERT_EQ(OB_SUCCESS, reader.rescan());
  alloc.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
  EXPECT_EQ(1, row.at(0).get_int());
}

TEST(FileSqlJsonlStream, RejectsNestedValuesAndDuplicateKeys)
{
  ObFileScanOptions opts;
  TempFile nested("{\"id\":1,\"bad\":{\"x\":2}}\n");
  FileJsonlStream nested_reader;
  EXPECT_NE(OB_SUCCESS, nested_reader.init(nested.open(), ObString(nested.path().c_str()), opts));
  TempFile duplicate("{\"id\":1,\"id\":2}\n");
  FileJsonlStream duplicate_reader;
  EXPECT_NE(OB_SUCCESS, duplicate_reader.init(duplicate.open(), ObString(duplicate.path().c_str()), opts));
}

TEST(FileSqlParquet, ReadsMultipleRowGroupsAndNullableColumns)
{
  const std::string source_path(__FILE__);
  const std::string data_dir = source_path.substr(0, source_path.find_last_of('/')) + "/data/";
  const char *fixtures[] = {"multicol-plain.parquet", "multicol-snappy-dict.parquet", "multicol-v2.parquet"};
  for (const char *fixture : fixtures) {
    SCOPED_TRACE(fixture);
    const std::string path = data_dir + fixture;
    ObArenaAllocator alloc;
    ObFileScanResult result;
    ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::scan_parquet(alloc, ObString(path.c_str()), result));
    ASSERT_EQ(4, result.row_count_);
    ASSERT_EQ(4, result.column_count());
    ASSERT_EQ(16, result.cells_.count());
    EXPECT_EQ(1, result.cells_.at(0).get_int());
    EXPECT_EQ(ObString("alpha"), result.cells_.at(1).get_string());
    EXPECT_DOUBLE_EQ(1.25, result.cells_.at(2).get_double());
    EXPECT_EQ(1, result.cells_.at(3).get_tinyint());
    EXPECT_EQ(2, result.cells_.at(4).get_int());
    EXPECT_TRUE(result.cells_.at(5).is_null());
    EXPECT_DOUBLE_EQ(2.5, result.cells_.at(6).get_double());
    EXPECT_EQ(0, result.cells_.at(7).get_tinyint());
    EXPECT_TRUE(result.cells_.at(8).is_null());
    EXPECT_EQ(ObString("中"), result.cells_.at(9).get_string());
    EXPECT_DOUBLE_EQ(3.75, result.cells_.at(10).get_double());
    EXPECT_TRUE(result.cells_.at(11).is_null());
    EXPECT_EQ(4, result.cells_.at(12).get_int());
    EXPECT_EQ(ObString("delta"), result.cells_.at(13).get_string());
    EXPECT_TRUE(result.cells_.at(14).is_null());
    EXPECT_EQ(1, result.cells_.at(15).get_tinyint());
  }
}

TEST(FileSqlParquet, ReadsLogicalDateAndNanosecondTimestamp)
{
  const std::string source_path(__FILE__);
  const std::string path = source_path.substr(0, source_path.find_last_of('/')) + "/data/temporal.parquet";
  ObArenaAllocator alloc;
  ObFileScanResult result;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::scan_parquet(alloc, ObString(path.c_str()), result));
  ASSERT_EQ(2, result.row_count_);
  ASSERT_EQ(3, result.column_count());
  ASSERT_EQ(6, result.cells_.count());
  EXPECT_TRUE(result.cells_.at(0).is_date());
  EXPECT_EQ(20666, result.cells_.at(0).get_date());
  EXPECT_TRUE(result.cells_.at(1).is_datetime());
  EXPECT_EQ(1785579072123456LL, result.cells_.at(1).get_datetime());
  EXPECT_TRUE(result.cells_.at(2).is_time());
  EXPECT_EQ(3661000000LL, result.cells_.at(2).get_time());
  EXPECT_TRUE(result.cells_.at(3).is_null());
  EXPECT_TRUE(result.cells_.at(4).is_null());
  EXPECT_TRUE(result.cells_.at(5).is_null());
}

TEST(FileSqlParquetStream, ReadsAndRescansAcrossPagesAndRowGroups)
{
  const std::string source_path(__FILE__);
  const std::string dir = source_path.substr(0, source_path.find_last_of('/')) + "/data/";
  const char *fixtures[] = {"multicol-plain.parquet", "multicol-snappy-dict.parquet", "multicol-v2.parquet"};
  for (const char *fixture : fixtures) {
    SCOPED_TRACE(fixture);
    const std::string path = dir + fixture;
    FileParquetStream reader;
    ASSERT_EQ(OB_SUCCESS, reader.init(fopen(path.c_str(), "rb"), ObString(path.c_str())));
    ASSERT_EQ(4U, reader.columns().size());
    ASSERT_EQ(4, reader.row_count());
    ObArenaAllocator alloc;
    ObSEArray<ObObj, 8> row;
    ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
    ASSERT_EQ(4, row.count());
    EXPECT_EQ(1, row.at(0).get_int());
    EXPECT_EQ(ObString("alpha"), row.at(1).get_string());
    EXPECT_DOUBLE_EQ(1.25, row.at(2).get_double());
    EXPECT_EQ(1, row.at(3).get_tinyint());
    alloc.reuse();
    ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
    EXPECT_EQ(2, row.at(0).get_int());
    EXPECT_TRUE(row.at(1).is_null());
    EXPECT_DOUBLE_EQ(2.5, row.at(2).get_double());
    EXPECT_EQ(0, row.at(3).get_tinyint());
    alloc.reuse();
    ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
    EXPECT_TRUE(row.at(0).is_null());
    EXPECT_EQ(ObString("中"), row.at(1).get_string());
    EXPECT_DOUBLE_EQ(3.75, row.at(2).get_double());
    EXPECT_TRUE(row.at(3).is_null());
    alloc.reuse();
    ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
    EXPECT_EQ(4, row.at(0).get_int());
    EXPECT_EQ(ObString("delta"), row.at(1).get_string());
    EXPECT_TRUE(row.at(2).is_null());
    EXPECT_EQ(1, row.at(3).get_tinyint());
    EXPECT_EQ(OB_ITER_END, reader.next_row(alloc, row));
    ASSERT_EQ(OB_SUCCESS, reader.rescan());
    alloc.reuse();
    ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row));
    EXPECT_EQ(1, row.at(0).get_int());
  }
}

TEST(FileSqlParquetStream, DecodesLongSnappyOffsetsAndAllNullColumn)
{
  const std::string source_path(__FILE__);
  const std::string path = source_path.substr(0, source_path.find_last_of('/')) + "/data/snappy-offset.parquet";
  FileParquetStream reader;
  ASSERT_EQ(OB_SUCCESS, reader.init(fopen(path.c_str(), "rb"), ObString(path.c_str())));
  ASSERT_EQ(1000, reader.row_count());
  ObArenaAllocator alloc;
  ObSEArray<ObObj, 8> row;
  for (int64_t i = 0; i < 1000; ++i) {
    ASSERT_EQ(OB_SUCCESS, reader.next_row(alloc, row)) << i;
    ASSERT_EQ(2, row.count());
    EXPECT_EQ(i, row.at(0).get_int());
    EXPECT_TRUE(row.at(1).is_null());
    alloc.reuse();
  }
  EXPECT_EQ(OB_ITER_END, reader.next_row(alloc, row));
}
} // namespace
