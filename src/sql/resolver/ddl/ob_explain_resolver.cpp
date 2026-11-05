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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
const static int64_t EXPLAIN_DESCRIBE_MARKER = 5;

namespace oceanbase
{
namespace sql
{
// DESCRIBE SELECT ... support (DuckDB semantics): resolve the inner select,
// then rewrite the statement into an internal
//   select column_0 as `column_name`, ... from (values row(...)) t
// so the engine streams the schema rows as a normal result set.
static int file_sql_describe_type_name(const ObObjType type, bool from_file_table,
                                       ObSqlString &name)
{
  int ret = OB_SUCCESS;
  const char *basic = nullptr;
  switch (type) {
    case ObTinyIntType:
      basic = from_file_table ? "BOOLEAN" : "TINYINT";
      break;
    case ObIntType:
      basic = from_file_table ? "BIGINT" : "BIGINT";
      break;
    case ObDoubleType:
    case ObFloatType:
      basic = "DOUBLE";
      break;
    case ObTimeType:
      basic = "TIME";
      break;
    case ObDateType:
      basic = "DATE";
      break;
    case ObDateTimeType:
      basic = "DATETIME";
      break;
    default:
      basic = "VARCHAR";
      break;
  }
  ret = name.append(basic);
  return ret;
}

static int file_sql_append_escaped_literal(ObSqlString &sql, const ObString &text)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
      const char c = text.ptr()[i];
      if ('\'' == c) {
        ret = sql.append("''");
      } else if ('\\' == c) {
        ret = sql.append("\\\\");
      } else {
        char buf[2] = {c, '\0'};
        ret = sql.append(buf, 1);
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

static int file_sql_resolve_describe_select(const ParseNode &select_node,
                                            ObResolverParams &params,
                                            ObStmt *&resolved)
{
  int ret = OB_SUCCESS;
  resolved = NULL;
  ObStmt *child_stmt = NULL;
  ObResolver child_resolver(params);
  ObSqlString sql;
  if (OB_FAIL(child_resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                     select_node, child_stmt))) {
    LOG_WARN("describe select: resolve inner query failed", K(ret));
  } else if (OB_ISNULL(child_stmt) || !child_stmt->is_select_stmt()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("describe select: inner stmt is not select", K(ret));
  } else if (OB_FAIL(sql.append_fmt(
      "select column_0 as `column_name`, column_1 as `column_type`, "
      "column_2 as `null`, column_3 as `key`, column_4 as `default`, "
      "column_5 as `extra` from (values "))) {
  } else {
    const ObSelectStmt *sel = static_cast<const ObSelectStmt *>(child_stmt);
    const ObIArray<SelectItem> &items = sel->get_select_items();
    for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
      const SelectItem &item = items.at(i);
      const ObRawExpr *expr = item.expr_;
      bool from_file = false;
      int64_t file_column_idx = -1;
      const TableItem *file_table = nullptr;
      ObString column_name = item.alias_name_;
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        if (column_name.empty()) {
          if (expr->is_column_ref_expr()) {
            const ObColumnRefRawExpr *col = static_cast<const ObColumnRefRawExpr *>(expr);
            column_name = col->get_column_name();
            file_table = sel->get_table_item_by_id(col->get_table_id());
            file_column_idx = col->get_column_id() - OB_APP_MIN_COLUMN_ID;
          }
        } else {
          // alias may hide the file column; still try to expose nullability
          if (expr->is_column_ref_expr()) {
            const ObColumnRefRawExpr *col = static_cast<const ObColumnRefRawExpr *>(expr);
            file_table = sel->get_table_item_by_id(col->get_table_id());
            file_column_idx = col->get_column_id() - OB_APP_MIN_COLUMN_ID;
          }
        }
        from_file = (nullptr != file_table && file_table->is_file_table()
                     && nullptr != file_table->values_table_def_
                     && file_column_idx >= 0
                     && file_column_idx < file_table->values_table_def_->column_types_.count());
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(sql.append("row("))) {
        } else if (OB_FAIL(file_sql_append_escaped_literal(sql, column_name))) {
        } else if (OB_FAIL(sql.append(","))) {
        } else {
          ObObjType type = expr->get_result_type().get_type();
          bool nullable = true;
          if (from_file) {
            const ObValuesTableDef *def = file_table->values_table_def_;
            type = def->column_types_.at(file_column_idx).get_type();
            nullable = (file_column_idx < def->column_nnvs_.count())
                       ? def->column_nnvs_.at(file_column_idx) > 0 : true;
          }
          ObSqlString type_name;
          if (OB_FAIL(file_sql_describe_type_name(type, from_file, type_name))) {
          } else if (OB_FAIL(file_sql_append_escaped_literal(sql, type_name.string()))) {
          } else if (OB_FAIL(sql.append_fmt(",%s,'NULL','NULL','NULL')",
                                            nullable ? "'YES'" : "'NO'"))) {
          }
        }
      }
      if (OB_SUCC(ret) && i + 1 < items.count()) {
        ret = sql.append(",");
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append(") as __describe_select__"))) {
      } else {
        ObParser parser(*params.allocator_, params.session_info_->get_sql_mode(),
                        params.session_info_->get_charsets4parser());
        ParseResult parse_result;
        ObStmt *describe_stmt = NULL;
        if (OB_FAIL(parser.parse(sql.string(), parse_result))) {
          LOG_WARN("describe select: parse generated sql failed", K(ret), K(sql.string()));
        } else if (OB_ISNULL(parse_result.result_tree_)
                   || OB_ISNULL(parse_result.result_tree_->children_[0])) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ObResolver resolver(params);
          if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                       *parse_result.result_tree_->children_[0],
                                       describe_stmt))) {
            LOG_WARN("describe select: resolve generated sql failed", K(ret));
          } else if (OB_ISNULL(describe_stmt)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            resolved = describe_stmt;
          }
        }
      }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  const bool is_describe_select = (6 == parse_tree.num_child_
                                   && NULL != parse_tree.children_[EXPLAIN_DESCRIBE_MARKER]
                                   && T_DESCRIBE_SELECT == parse_tree.children_[EXPLAIN_DESCRIBE_MARKER]->type_
                                   && NULL != parse_tree.children_[EXPLAIN_CHILD_STMT]
                                   && T_SELECT == parse_tree.children_[EXPLAIN_CHILD_STMT]->type_);
  if (is_describe_select) {
    // `DESCRIBE SELECT ...` / `DESC SELECT ...` returns the query schema as a
    // normal result set instead of a query plan.
    ObStmt *describe_stmt = NULL;
    if (OB_FAIL(file_sql_resolve_describe_select(*parse_tree.children_[EXPLAIN_CHILD_STMT],
                                                 params_, describe_stmt))) {
      LOG_WARN("resolve DESCRIBE SELECT failed", K(ret));
    } else {
      set_basic_stmt(describe_stmt);
    }
  } else if (!(T_EXPLAIN == parse_tree.type_
               && (5 == parse_tree.num_child_ || 6 == parse_tree.num_child_))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
