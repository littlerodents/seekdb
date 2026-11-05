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

#include "sql/engine/dml/ob_ai_column_dml_service.h"
#include "lib/hash/ob_md5.h"
#include "lib/time/ob_time_utility.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/ob_sql_context.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_ai_column_sql_service.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{

namespace ob_ai
{

int escape_string(const ObString &src, ObSqlString &dst)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < src.length(); ++i) {
    const char c = src.ptr()[i];
    if ('\'' == c) {
      if (OB_FAIL(dst.append("''"))) {
      }
    } else if ('\\' == c) {
      if (OB_FAIL(dst.append("\\\\"))) {
      }
    } else if (OB_FAIL(dst.append(c))) {
    }
  }
  return ret;
}

int obj_to_sql_literal(const ObObj &obj, ObSqlString &dst)
{
  int ret = OB_SUCCESS;
  if (obj.is_null()) {
    ret = dst.append("NULL");
  } else if (obj.is_integer_type() || obj.is_float() || obj.is_double()
             || obj.is_numeric_type()) {
    ObString num_str;
    if (OB_FAIL(obj.get_varchar(num_str))) {
      LOG_WARN("fail to get numeric text", K(ret), K(obj));
    } else if (OB_FAIL(dst.append(num_str))) {
    }
  } else {
    ObString text;
    if (OB_FAIL(obj.get_string(text))) {
      LOG_WARN("fail to get text form", K(ret), K(obj));
    } else if (OB_FAIL(dst.append('\''))) {
    } else if (OB_FAIL(escape_string(text, dst))) {
    } else if (OB_FAIL(dst.append('\''))) {
    }
  }
  return ret;
}

void hex_encode(const ObString &src, ObSqlString &dst)
{
  static const char *hex_chars = "0123456789abcdef";
  for (int64_t i = 0; i < src.length(); ++i) {
    const uint8_t b = static_cast<uint8_t>(src.ptr()[i]);
    dst.append(hex_chars[(b >> 4) & 0xF]);
    dst.append(hex_chars[b & 0xF]);
  }
}

int md5_hex(const ObString &seed, char *buf, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  ObMd5 md5;
  unsigned char digest[16] = {0};
  if (OB_ISNULL(buf) || buf_len < 33) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    md5.update(seed.ptr(), static_cast<int32_t>(seed.length()));
    md5.final(digest);
    static const char *hex_chars = "0123456789abcdef";
    for (int64_t i = 0; i < 16; ++i) {
      buf[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
      buf[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    buf[32] = '\0';
  }
  return ret;
}

int get_table_schema(ObExecContext &ctx, const uint64_t table_id,
                     const ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  table_schema = NULL;
  const ObSqlCtx *sql_ctx = ctx.get_sql_ctx();
  ObSchemaGetterGuard *guard = OB_NOT_NULL(sql_ctx) ? sql_ctx->schema_guard_ : NULL;
  if (OB_ISNULL(guard)) {
    // no schema guard available (e.g. inner statements); skip
  } else if (OB_FAIL(guard->get_table_schema(table_id, table_schema))) {
    LOG_WARN("fail to get table schema", K(ret), K(table_id));
  }
  return ret;
}

int open_nested_conn(ObSQLSessionInfo *session,
                     sqlclient::ObISQLConnectionGuard &conn_guard,
                     sqlclient::ObISQLConnection *&conn)
{
  int ret = OB_SUCCESS;
  conn = NULL;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(query::ObInnerSQLConnectionAccess::
                     create_connection_with_external_session(session, conn_guard))) {
    LOG_WARN("fail to create nested connection on user session", K(ret));
  } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int hidden_names(const ObString &column_name,
                 ObSqlString &state_col,
                 ObSqlString &request_col,
                 ObSqlString &result_spec_col)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(state_col.assign_fmt("__ai_%.*s_state",
                                   static_cast<int>(column_name.length()), column_name.ptr()))) {
  } else if (OB_FAIL(request_col.assign_fmt("__ai_%.*s_request",
                                            static_cast<int>(column_name.length()), column_name.ptr()))) {
  } else if (OB_FAIL(result_spec_col.assign_fmt("__ai_%.*s_result_spec",
                                                static_cast<int>(column_name.length()), column_name.ptr()))) {
  }
  return ret;
}

} // end namespace ob_ai

int ObAIColumnDMLService::on_write_op_open(ObExecContext &ctx,
                                           const uint64_t table_id,
                                           common::ObIArray<common::ObString> &pk_names,
                                           bool &hook_active)
{
  int ret = OB_SUCCESS;
  hook_active = false;
  pk_names.reset();
  const ObTableSchema *table_schema = NULL;
  if (OB_FAIL(ob_ai::get_table_schema(ctx, table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema) || !table_schema->has_ai_column()) {
    // not an AI table: nothing to do
  } else {
    const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
      uint64_t col_id = OB_INVALID_ID;
      const ObColumnSchemaV2 *col = NULL;
      if (OB_FAIL(rowkey_info.get_column_id(i, col_id))) {
      } else if (OB_ISNULL(col = table_schema->get_column_schema(col_id))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        common::ObString saved;
        if (OB_FAIL(ob_write_string(ctx.get_allocator(), col->get_column_name_str(), saved))) {
        } else if (OB_FAIL(pk_names.push_back(saved))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      hook_active = true;
    }
  }
  return ret;
}

int ObAIColumnDMLService::collect_written_rowkey(common::ObIAllocator &alloc,
                                                 ObEvalCtx &eval_ctx,
                                                 const common::ObIArray<common::ObString> &pk_names,
                                                 const ObExprPtrIArray &full_row,
                                                 const int64_t rowkey_cnt,
                                                 common::ObIArray<common::ObString> &rowkey_preds,
                                                 common::ObIArray<common::ObString> &rowkey_encs)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(rowkey_cnt <= 0 || rowkey_cnt != pk_names.count()
                  || full_row.count() < rowkey_cnt)) {
    // skip rows with unexpected layout
  } else {
    ObSqlString pred;
    ObSqlString enc;
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
      const ObExpr *expr = full_row.at(i);
      ObDatum *datum = NULL;
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(expr->eval(eval_ctx, datum))) {
        LOG_WARN("fail to eval rowkey expr", K(ret));
      } else if (OB_ISNULL(datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObObj obj;
        if (OB_FAIL(datum->to_obj(obj, expr->obj_meta_, expr->datum_meta_))) {
          LOG_WARN("fail to convert datum to obj", K(ret));
        } else {
          ObSqlString literal;
          if (OB_FAIL(ob_ai::obj_to_sql_literal(obj, literal))) {
          } else {
            // text form for encoding
            ObSqlString text;
            if (obj.is_null()) {
              text.append("NULL");
            } else if (obj.is_integer_type() || obj.is_float() || obj.is_double()
                       || obj.is_numeric_type()) {
              text.append(literal.string());
            } else {
              ObString sv;
              if (OB_SUCC(obj.get_string(sv))) {
                text.append(sv);
              }
            }
            if (OB_FAIL(ret)) {
            } else if (i > 0 && OB_FAIL(pred.append(" AND "))) {
            } else if (OB_FAIL(pred.append_fmt("`%.*s` = ",
                                               static_cast<int>(pk_names.at(i).length()),
                                               pk_names.at(i).ptr()))) {
            } else if (OB_FAIL(pred.append(literal.string()))) {
            } else if (i > 0 && OB_FAIL(enc.append(':'))) {
            } else if (OB_FAIL(ob_ai::hex_encode(text.string(), enc))) {
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      common::ObString pred_saved;
      common::ObString enc_saved;
      if (OB_FAIL(ob_write_string(alloc, pred.string(), pred_saved))) {
      } else if (OB_FAIL(ob_write_string(alloc, enc.string(), enc_saved))) {
      } else if (OB_FAIL(rowkey_preds.push_back(pred_saved))) {
      } else if (OB_FAIL(rowkey_encs.push_back(enc_saved))) {
      }
    }
  }
  return ret;
}

int ObAIColumnDMLService::finish_write(ObExecContext &ctx,
                                       const uint64_t table_id,
                                       const bool is_delete_op,
                                       const bool is_insert_op,
                                       const common::ObIArray<common::ObString> &rowkey_preds,
                                       const common::ObIArray<common::ObString> &rowkey_encs)
{
  int ret = OB_SUCCESS;
  if (rowkey_preds.empty()) {
    // nothing touched
  } else {
    const ObTableSchema *table_schema = NULL;
    if (OB_FAIL(ob_ai::get_table_schema(ctx, table_id, table_schema))) {
    } else if (OB_ISNULL(table_schema) || !table_schema->has_ai_column()) {
      // not an AI table
    } else {
      const ObString &db_name = table_schema->get_database_name_str();
      const ObString &tb_name = table_schema->get_table_name_str();
      if (db_name.empty() || tb_name.empty()) {
        // cannot address the table
      } else {
        sqlclient::ObISQLConnectionGuard conn_guard;
        sqlclient::ObISQLConnection *conn = NULL;
        int64_t affected = 0;
        if (OB_FAIL(ob_ai::open_nested_conn(ctx.get_my_session(), conn_guard, conn))) {
          LOG_WARN("fail to open nested conn for ai column maintenance", K(ret));
        } else if (is_delete_op) {
          // drop pending tasks of deleted rows
          for (int64_t r = 0; OB_SUCC(ret) && r < rowkey_encs.count(); ++r) {
            ObSqlString sql;
            if (OB_FAIL(sql.assign_fmt(
                    "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND rowkey_enc = '%s'",
                    table_id, rowkey_encs.at(r).ptr()))) {
            } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
              LOG_WARN("fail to delete ai column tasks on row delete", K(ret), K(sql));
            }
          }
        } else {
          const int64_t stmt_token = ObTimeUtility::current_time();
          const ObIArray<ObAIColumnInfo> &ai_columns = table_schema->get_ai_columns();
          // 1) create/refresh task rows first (visible in the user transaction)
          for (int64_t r = 0; OB_SUCC(ret) && r < rowkey_preds.count(); ++r) {
            for (int64_t c = 0; OB_SUCC(ret) && c < ai_columns.count(); ++c) {
              const ObAIColumnInfo &ai_info = ai_columns.at(c);
              char request_id[64] = {0};
              ObSqlString seed;
              ObSqlString request_col;
              if (OB_FAIL(request_col.assign_fmt("__ai_%.*s_request",
                                                 static_cast<int>(ai_info.column_name_.length()),
                                                 ai_info.column_name_.ptr()))) {
              } else if (OB_FAIL(seed.append(rowkey_preds.at(r)))) {
              } else if (OB_FAIL(seed.append_fmt("|%ld", stmt_token))) {
              } else if (OB_FAIL(ob_ai::md5_hex(seed.string(), request_id, sizeof(request_id)))) {
              } else {
                ObSqlString sql;
                if (OB_FAIL(sql.assign_fmt(
                        "INSERT INTO oceanbase.__all_ai_column_task "
                        "(table_id, column_id, rowkey_enc, request_id, rowkey_predicate, "
                        "state, retry_count, eligible_ts, request) "
                        "SELECT %lu, %lu, '%s', '%s', '%s', 0, 0, CURRENT_TIMESTAMP(6), (%s) "
                        "FROM `%.*s`.`%.*s` WHERE %s "
                        "AND NOT (`%s` <=> (%s)) "
                        "ON DUPLICATE KEY UPDATE state = 0, retry_count = 0, "
                        "eligible_ts = VALUES(eligible_ts), request = VALUES(request), "
                        "request_id = VALUES(request_id), rowkey_predicate = VALUES(rowkey_predicate)",
                        table_id, ai_info.column_id_,
                        rowkey_encs.at(r).ptr(), request_id,
                        rowkey_preds.at(r).ptr(),
                        ai_info.normalized_expr_.ptr(),
                        static_cast<int>(db_name.length()), db_name.ptr(),
                        static_cast<int>(tb_name.length()), tb_name.ptr(),
                        rowkey_preds.at(r).ptr(),
                        request_col.ptr(),
                        ai_info.normalized_expr_.ptr()))) {
                  LOG_WARN("fail to build task upsert sql", K(ret));
                } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
                  LOG_WARN("fail to upsert ai column task", K(ret), K(sql));
                }
              }
            }
          }
          // 2) invalidate results: null the column, reset state, refresh request
          for (int64_t c = 0; OB_SUCC(ret) && c < ai_columns.count(); ++c) {
            const ObAIColumnInfo &ai_info = ai_columns.at(c);
            ObSqlString state_col;
            ObSqlString request_col;
            ObSqlString result_spec_col;
            if (OB_FAIL(ob_ai::hidden_names(ai_info.column_name_, state_col, request_col, result_spec_col))) {
            } else {
              for (int64_t r = 0; OB_SUCC(ret) && r < rowkey_preds.count(); ++r) {
                // insert: invalidate whenever the result does not match the fresh request
                // update: only when a previously ready result became stale
                ObSqlString sql;
                if (is_insert_op) {
                  if (OB_FAIL(sql.assign_fmt(
                          "UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%s` = 0, `%s` = NULL, `%s` = (%s) "
                          "WHERE %s AND NOT (`%s` <=> (%s))",
                          static_cast<int>(db_name.length()), db_name.ptr(),
                          static_cast<int>(tb_name.length()), tb_name.ptr(),
                          static_cast<int>(ai_info.column_name_.length()), ai_info.column_name_.ptr(),
                          state_col.ptr(), result_spec_col.ptr(), request_col.ptr(),
                          ai_info.normalized_expr_.ptr(),
                          rowkey_preds.at(r).ptr(),
                          result_spec_col.ptr(),
                          ai_info.normalized_expr_.ptr()))) {
                  }
                } else {
                  if (OB_FAIL(sql.assign_fmt(
                          "UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%s` = 0, `%s` = NULL, `%s` = (%s) "
                          "WHERE %s AND `%s` IS NOT NULL AND NOT (`%s` <=> (%s))",
                          static_cast<int>(db_name.length()), db_name.ptr(),
                          static_cast<int>(tb_name.length()), tb_name.ptr(),
                          static_cast<int>(ai_info.column_name_.length()), ai_info.column_name_.ptr(),
                          state_col.ptr(), result_spec_col.ptr(), request_col.ptr(),
                          ai_info.normalized_expr_.ptr(),
                          rowkey_preds.at(r).ptr(),
                          result_spec_col.ptr(),
                          result_spec_col.ptr(),
                          ai_info.normalized_expr_.ptr()))) {
                  }
                }
                if (OB_SUCC(ret)) {
                  if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
                    LOG_WARN("fail to invalidate ai column", K(ret), K(sql));
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnDMLService::execute_ai_column_retry(ObExecContext &ctx,
                                                  const uint64_t table_id,
                                                  const common::ObString &column_name,
                                                  const common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = NULL;
  if (OB_FAIL(ob_ai::get_table_schema(ctx, table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else {
    const ObString &db_name = table_schema->get_database_name_str();
    const ObString &tb_name = table_schema->get_table_name_str();
    const ObAIColumnInfo *ai_info = NULL;
    for (int64_t i = 0; OB_ISNULL(ai_info) && i < table_schema->get_ai_columns().count(); ++i) {
      if (0 == column_name.case_compare(table_schema->get_ai_columns().at(i).column_name_)) {
        ai_info = &table_schema->get_ai_columns().at(i);
      }
    }
    if (OB_ISNULL(ai_info)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
    } else if (db_name.empty() || tb_name.empty()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sqlclient::ObISQLConnectionGuard conn_guard;
      sqlclient::ObISQLConnection *conn = NULL;
      int64_t affected = 0;
      const int64_t stmt_token = ObTimeUtility::current_time();
      if (OB_FAIL(ob_ai::open_nested_conn(ctx.get_my_session(), conn_guard, conn))) {
      } else {
        // build rowkey expression text for rowkey_enc / rowkey_predicate columns in SQL
        ObSqlString pred_expr;
        ObSqlString enc_expr;
        const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
        for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
          uint64_t col_id = OB_INVALID_ID;
          const ObColumnSchemaV2 *col = NULL;
          if (OB_FAIL(rowkey_info.get_column_id(i, col_id))) {
          } else if (OB_ISNULL(col = table_schema->get_column_schema(col_id))) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const ObString &name = col->get_column_name_str();
            if (i > 0) {
              if (OB_FAIL(pred_expr.append(", ' AND ', "))) {
              } else if (OB_FAIL(enc_expr.append(", "))) {
              }
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(pred_expr.append_fmt("CONCAT('`%.*s` = ', CAST(`%.*s` AS CHAR))",
                                                    static_cast<int>(name.length()), name.ptr(),
                                                    static_cast<int>(name.length()), name.ptr()))) {
            } else if (OB_FAIL(enc_expr.append_fmt("LOWER(HEX(CAST(`%.*s` AS CHAR)))",
                                                   static_cast<int>(name.length()), name.ptr()))) {
            }
          }
        }
        ObSqlString pred_concat;
        ObSqlString enc_concat;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(pred_concat.assign("CONCAT("))
                   || OB_FAIL(pred_concat.append(pred_expr.string()))
                   || OB_FAIL(pred_concat.append(")"))) {
        } else if (OB_FAIL(enc_concat.assign("CONCAT_WS(':', "))
                   || OB_FAIL(enc_concat.append(enc_expr.string()))
                   || OB_FAIL(enc_concat.append(")"))) {
        } else {
          // task upsert for all rows matching the predicate
          ObSqlString sql;
          if (OB_FAIL(sql.assign_fmt(
                  "INSERT INTO oceanbase.__all_ai_column_task "
                  "(table_id, column_id, rowkey_enc, request_id, rowkey_predicate, "
                  "state, retry_count, eligible_ts, request) "
                  "SELECT %lu, %lu, %s, MD5(CONCAT_WS(':', %s, '%ld')), %s, 0, 0, CURRENT_TIMESTAMP(6), (%s) "
                  "FROM `%.*s`.`%.*s` WHERE %s "
                  "ON DUPLICATE KEY UPDATE state = 0, retry_count = 0, "
                  "eligible_ts = VALUES(eligible_ts), request = VALUES(request), "
                  "request_id = VALUES(request_id), rowkey_predicate = VALUES(rowkey_predicate)",
                  table_id, ai_info->column_id_,
                  enc_concat.ptr(),
                  enc_expr.ptr(), stmt_token,
                  pred_concat.ptr(),
                  ai_info->normalized_expr_.ptr(),
                  static_cast<int>(db_name.length()), db_name.ptr(),
                  static_cast<int>(tb_name.length()), tb_name.ptr(),
                  predicate.empty() ? "1 = 1" : predicate.ptr()))) {
          } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
            LOG_WARN("fail to upsert ai column tasks for retry", K(ret), K(sql));
          }
        }
        if (OB_SUCC(ret)) {
          // invalidate the selected rows
          ObSqlString state_col;
          ObSqlString request_col;
          ObSqlString result_spec_col;
          if (OB_FAIL(ob_ai::hidden_names(column_name, state_col, request_col, result_spec_col))) {
          } else {
            ObSqlString sql;
            if (OB_FAIL(sql.assign_fmt(
                    "UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%s` = 0, `%s` = NULL, `%s` = (%s) "
                    "WHERE %s",
                    static_cast<int>(db_name.length()), db_name.ptr(),
                    static_cast<int>(tb_name.length()), tb_name.ptr(),
                    static_cast<int>(column_name.length()), column_name.ptr(),
                    state_col.ptr(), result_spec_col.ptr(), request_col.ptr(),
                    ai_info->normalized_expr_.ptr(),
                    predicate.empty() ? "1 = 1" : predicate.ptr()))) {
            } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
              LOG_WARN("fail to invalidate ai column for retry", K(ret), K(sql));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnDMLService::check_strict_read(ObExecContext &ctx,
                                            const common::ObIArray<uint64_t> &table_ids,
                                            const common::ObIArray<common::ObString> &column_names)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (table_ids.empty() || OB_ISNULL(session)) {
    // nothing referenced
  } else {
    sqlclient::ObISQLConnectionGuard conn_guard;
    sqlclient::ObISQLConnection *conn = NULL;
    if (OB_FAIL(ob_ai::open_nested_conn(session, conn_guard, conn))) {
      LOG_WARN("fail to open nested conn for strict read check", K(ret));
    } else {
      // one probe per referenced column
      for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
        SMART_VAR(ObMySQLProxy::MySQLResult, res)
        {
          ObMySQLResult *result = NULL;
          const ObTableSchema *table_schema = NULL;
          if (OB_FAIL(ob_ai::get_table_schema(ctx, table_ids.at(i), table_schema))) {
          } else if (OB_ISNULL(table_schema)) {
            // table vanished; let the main query report it
          } else {
            const ObString &db_name = table_schema->get_database_name_str();
            const ObString &tb_name = table_schema->get_table_name_str();
            ObSqlString state_col;
            ObSqlString request_col;
            ObSqlString result_spec_col;
            if (OB_FAIL(ob_ai::hidden_names(column_names.at(i), state_col, request_col, result_spec_col))) {
            } else {
              ObSqlString sql;
              if (OB_FAIL(sql.assign_fmt(
                      "SELECT 1 FROM `%.*s`.`%.*s` WHERE `%s` IS NULL OR `%s` <> 2 LIMIT 1",
                      static_cast<int>(db_name.length()), db_name.ptr(),
                      static_cast<int>(tb_name.length()), tb_name.ptr(),
                      state_col.ptr(), state_col.ptr()))) {
              } else if (OB_FAIL(conn->execute_read(sql.ptr(), res))) {
                LOG_WARN("fail to probe ai column state", K(ret), K(sql));
              } else if (OB_ISNULL(result = res.get_result())) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_ITER_END == (ret = result->next())) {
                // all rows ready
                ret = OB_SUCCESS;
              } else if (OB_SUCC(ret)) {
                // a row is not READY: strict read violation
                ret = OB_AI_COLUMN_UNFINISHED_TASK;
                LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASK,
                               table_ids.at(i), table_schema->get_ai_column_by_column_id(
                                   table_schema->get_column_schema(column_names.at(i))
                                       ? table_schema->get_column_schema(column_names.at(i))->get_column_id()
                                       : 0));
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
