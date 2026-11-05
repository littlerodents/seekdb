/*
 * Copyright (c) 2026 OceanBase.
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

#include <gtest/gtest.h>
#include <string.h>
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "common/sql_mode/ob_sql_mode.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{
class FileSqlParserTest : public ::testing::Test
{
public:
  FileSqlParserTest() : allocator_(ObModIds::OB_SQL_PARSER) {}
  virtual ~FileSqlParserTest() {}
  virtual void SetUp() override {}
  virtual void TearDown() override { allocator_.reset(); }
  // returns the stmt-level node or NULL when parsing fails
  const ParseNode *parse_ok(const char *sql)
  {
    int ret = OB_SUCCESS;
    ParseResult parse_result;
    ObParser parser(allocator_, SMO_DEFAULT, ObCharsets4Parser());
    if (OB_FAIL(parser.parse(ObString(sql), parse_result))) {
      EXPECT_EQ(OB_SUCCESS, ret) << sql;
      return NULL;
    }
    if (NULL == parse_result.result_tree_ || NULL == parse_result.result_tree_->children_[0]) {
      EXPECT_TRUE(false) << sql;
      return NULL;
    }
    return parse_result.result_tree_->children_[0];
  }

  void parse_fail(const char *sql)
  {
    int ret = OB_SUCCESS;
    ParseResult parse_result;
    ObParser parser(allocator_, SMO_DEFAULT, ObCharsets4Parser());
    ret = parser.parse(ObString(sql), parse_result);
    EXPECT_NE(OB_SUCCESS, ret) << sql;
  }

  ObArenaAllocator allocator_;
};

const ParseNode *find_first(const ParseNode *node, ObItemType type)
{
  if (NULL == node) {
    return NULL;
  }
  if (type == node->type_) {
    return node;
  }
  const ParseNode *found = NULL;
  for (int i = 0; NULL == found && i < node->num_child_; ++i) {
    found = find_first(node->children_[i], type);
  }
  return found;
}

TEST_F(FileSqlParserTest, ParsesReadFunctionsAsTableFactors)
{
  const char *sqls[] = {
      "select * from read_csv('/tmp/a.csv')",
      "select * from read_csv('/tmp/a.csv') f",
      "select * from read_csv('/tmp/a.csv') as f",
      "select * from read_json('/tmp/a.json', format = 'newline_delimited') f",
      "select * from read_ndjson('/tmp/a.jsonl')",
      "select * from read_parquet('/tmp/a.parquet') p",
      "select count(*) from read_csv('/tmp/a.csv', header = true, sample_size = -1) f",
      "select * from glob('/tmp/*')",
      "select * from glob('/tmp/*') g",
      "select f.id from read_csv('/tmp/a.csv', 'csv', delim = ';') f where f.id > 1",
      "select a.id, b.name from read_csv('/tmp/a.csv') a join read_csv('/tmp/b.csv') b on a.id = b.id",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    const ParseNode *stmt = parse_ok(sqls[i]);
    ASSERT_TRUE(NULL != stmt) << sqls[i];
    const ParseNode *table = find_first(stmt, T_FILE_TABLE_EXPRESSION);
    ASSERT_TRUE(NULL != table) << sqls[i];
    ASSERT_EQ(5, table->num_child_);
    ASSERT_TRUE(NULL != table->children_[0]);
    ASSERT_TRUE(NULL != table->children_[1]);
    ASSERT_TRUE(NULL != table->children_[1]->str_value_);
  }
}

TEST_F(FileSqlParserTest, ReadFunctionKindValues)
{
  struct Case { const char *sql; int64_t kind; };
  const Case cases[] = {
      {"select * from read_csv('/a')", 0},
      {"select * from read_json('/a')", 1},
      {"select * from read_ndjson('/a')", 2},
      {"select * from read_parquet('/a')", 3},
      {"select * from glob('/a/*')", 4},
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(cases) / sizeof(cases[0])); ++i) {
    const ParseNode *stmt = parse_ok(cases[i].sql);
    ASSERT_TRUE(NULL != stmt);
    const ParseNode *table = find_first(stmt, T_FILE_TABLE_EXPRESSION);
    ASSERT_TRUE(NULL != table);
    ASSERT_TRUE(NULL != table->children_[0]);
    EXPECT_EQ(cases[i].kind, table->children_[0]->value_);
  }
}

TEST_F(FileSqlParserTest, ParsesFileOptions)
{
  const ParseNode *stmt = parse_ok(
      "select * from read_csv('/a', header = false, sample_size = -1, "
      "delim = ',', quote = '\"', escape = '\"', nullstr = 'NULL', skip = 2) t");
  ASSERT_TRUE(NULL != stmt);
  const ParseNode *table = find_first(stmt, T_FILE_TABLE_EXPRESSION);
  ASSERT_TRUE(NULL != table);
  // options form a left-nested T_LINK_NODE chain under children_[2]:
  // T_LINK_NODE(prefix, latest_option)
  int option_cnt = 0;
  const ParseNode *cur = table->children_[2];
  while (NULL != cur) {
    if (T_FILE_SQL_OPTION == cur->type_) {
      ++option_cnt;
      cur = NULL;
    } else if (T_LINK_NODE == cur->type_) {
      if (NULL != cur->children_[1]) {
        ++option_cnt;
      }
      cur = cur->children_[0];
    } else {
      cur = NULL;
      FAIL() << "unexpected option chain node";
    }
  }
  EXPECT_EQ(7, option_cnt);
}

TEST_F(FileSqlParserTest, CopyRewritesToSelectWithIntoOutfile)
{
  const ParseNode *stmt = parse_ok(
      "copy (select id, name from read_csv('/a.csv') order by id) "
      "to '/tmp/out.csv' (format csv, header, nullstr 'NULL')");
  ASSERT_TRUE(NULL != stmt);
  ASSERT_EQ(T_SELECT, stmt->type_);
  const ParseNode *into = stmt->children_[PARSE_SELECT_INTO_EXTRA];
  ASSERT_TRUE(NULL != into);
  ASSERT_EQ(T_INTO_OUTFILE, into->type_);
  ASSERT_EQ(6, into->num_child_);
  ASSERT_EQ(1, into->value_);
  const ParseNode *opts = into->children_[5];
  ASSERT_TRUE(NULL != opts);
  ASSERT_EQ(T_FILE_SQL_OPTION_LIST, opts->type_);
  EXPECT_EQ(3, opts->num_child_);
}

TEST_F(FileSqlParserTest, CopyWithoutOptionsAndWithExpressions)
{
  const ParseNode *stmt = parse_ok("copy (select 1) to '/tmp/o.csv'");
  ASSERT_TRUE(NULL != stmt);
  ASSERT_EQ(T_SELECT, stmt->type_);
  const ParseNode *into = stmt->children_[PARSE_SELECT_INTO_EXTRA];
  ASSERT_TRUE(NULL != into);
  ASSERT_EQ(T_INTO_OUTFILE, into->type_);
  EXPECT_EQ(6, into->num_child_);
  EXPECT_TRUE(NULL == into->children_[5]);

  // nested parens and aggregates stay one statement
  const ParseNode *agg = parse_ok(
      "copy (select count(*), sum(id) from read_csv('/a.csv') f group by f.name) "
      "to '/tmp/o.csv' (format csv, header = false)");
  ASSERT_TRUE(NULL != agg);
  ASSERT_EQ(T_SELECT, agg->type_);
}

TEST_F(FileSqlParserTest, DescribeSelectCarriesMarker)
{
  const ParseNode *stmt = parse_ok("describe select * from read_csv('/a.csv')");
  ASSERT_TRUE(NULL != stmt);
  ASSERT_EQ(T_EXPLAIN, stmt->type_);
  ASSERT_EQ(6, stmt->num_child_);
  ASSERT_TRUE(NULL != stmt->children_[5]);
  EXPECT_EQ(T_DESCRIBE_SELECT, stmt->children_[5]->type_);
  ASSERT_TRUE(NULL != stmt->children_[4]);
  EXPECT_EQ(T_SELECT, stmt->children_[4]->type_);

  const ParseNode *desc = parse_ok("desc select * from read_csv('/a.csv')");
  ASSERT_TRUE(NULL != desc);
  ASSERT_EQ(T_EXPLAIN, desc->type_);
  ASSERT_EQ(6, desc->num_child_);
  EXPECT_EQ(T_DESCRIBE_SELECT, desc->children_[5]->type_);
}

TEST_F(FileSqlParserTest, ExplainSelectStaysPlainExplain)
{
  const ParseNode *stmt = parse_ok("explain select * from read_csv('/a.csv')");
  ASSERT_TRUE(NULL != stmt);
  ASSERT_EQ(T_EXPLAIN, stmt->type_);
  EXPECT_EQ(5, stmt->num_child_);
}

TEST_F(FileSqlParserTest, DescribeTableStillShowsColumns)
{
  const ParseNode *stmt = parse_ok("describe t1");
  ASSERT_TRUE(NULL != stmt);
  EXPECT_EQ(T_SHOW_COLUMNS, stmt->type_);
}

TEST_F(FileSqlParserTest, EnableFileSqlVariableSet)
{
  const ParseNode *stmt = parse_ok("set session enable_file_sql = on");
  ASSERT_TRUE(NULL != stmt);
  EXPECT_EQ(T_VARIABLE_SET, stmt->type_);
  parse_ok("set @@session.enable_file_sql = off");
  parse_ok("set global enable_file_sql = 1");
}

TEST_F(FileSqlParserTest, KeywordsRemainUsableAsIdentifiers)
{
  // non-reserved keywords must still work as ordinary names
  const ParseNode *s1 = parse_ok("select glob from t1");
  ASSERT_TRUE(NULL != s1);
  const ParseNode *s2 = parse_ok("select copy from t1");
  ASSERT_TRUE(NULL != s2);
}

TEST_F(FileSqlParserTest, RejectsMalformedFileFunctionSyntax)
{
  parse_fail("select * from read_csv()");
  parse_fail("select * from read_csv('/a', header = )");
  parse_fail("copy select 1 to '/a'");
  parse_fail("copy (select 1) to");
  parse_fail("copy (select 1) to '/a' (header = )");
}
} // namespace
