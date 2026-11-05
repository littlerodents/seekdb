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

#include "share/schema/ob_ai_column_sql_service.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/inner_table/ob_inner_table_schema.h"

#include "lib/ob_name_def.h"
#include "lib/time/ob_time_utility.h"

#define USING_LOG_PREFIX SHARE_SCHEMA

namespace oceanbase
{
namespace share
{
namespace schema
{

int ObAIColumnSqlService::hidden_ai_column_names(const ObString &column_name,
                                                 ObSqlString &state_name,
                                                 ObSqlString &request_name,
                                                 ObSqlString &result_spec_name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(state_name.assign_fmt("__ai_%.*s_state", column_name.length(), column_name.ptr()))) {
  } else if (OB_FAIL(request_name.assign_fmt("__ai_%.*s_request", column_name.length(), column_name.ptr()))) {
  } else if (OB_FAIL(result_spec_name.assign_fmt("__ai_%.*s_result_spec", column_name.length(), column_name.ptr()))) {
  }
  return ret;
}

int ObAIColumnSqlService::gen_ai_column_dml(const ObTableSchema &table_schema,
                                            const ObColumnSchemaV2 &column,
                                            const ObAIColumnInfo &ai_info,
                                            share::ObDMLSqlSplicer &dml)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(dml.add_pk_column("table_id", table_schema.get_table_id()))
      || OB_FAIL(dml.add_pk_column("column_id", column.get_column_id()))
      || OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(ai_info.column_name_)))
      || OB_FAIL(dml.add_column("state_column_id", ai_info.state_column_id_))
      || OB_FAIL(dml.add_column("request_column_id", ai_info.request_column_id_))
      || OB_FAIL(dml.add_column("result_spec_column_id", ai_info.result_spec_column_id_))
      || OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(ai_info.normalized_expr_)))
      || OB_FAIL(dml.add_column("ai_model_name", ObHexEscapeSqlStr(ai_info.ai_model_name_)))
      || OB_FAIL(dml.add_column("ai_func", ObHexEscapeSqlStr(ai_info.ai_func_)))
      || OB_FAIL(dml.add_column("sync_mode", ObHexEscapeSqlStr(ai_info.sync_mode_)))
      || OB_FAIL(dml.add_column("max_retries", ai_info.max_retries_))
      || OB_FAIL(dml.add_column("embed_dim", ai_info.embed_dim_))) {
    LOG_WARN("fail to add ai column dml columns", K(ret));
  } else if (OB_FAIL(dml.finish_row())) {
    LOG_WARN("fail to finish ai column dml row", K(ret));
  }
  return ret;
}

// Resolve one AI column's metadata: hidden column ids are discovered by name from the
// assembled table schema, so the DDL resolver does not need to track id assignment order.
int ObAIColumnSqlService::resolve_ai_column_info(const ObTableSchema &table_schema,
                                                 const ObColumnSchemaV2 &column,
                                                 ObAIColumnInfo &ai_info)
{
  int ret = OB_SUCCESS;
  ai_info.reset();
  if (OB_UNLIKELY(!column.is_ai_column())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("column is not an ai column", K(ret));
  } else {
    ai_info = column.get_ai_info();
    ai_info.table_id_ = table_schema.get_table_id();
    ai_info.column_id_ = column.get_column_id();
    ai_info.column_name_ = column.get_column_namestr();
    ObSqlString state_name;
    ObSqlString request_name;
    ObSqlString result_spec_name;
    const ObColumnSchemaV2 *state_col = NULL;
    const ObColumnSchemaV2 *request_col = NULL;
    const ObColumnSchemaV2 *result_spec_col = NULL;
    if (OB_FAIL(hidden_ai_column_names(column.get_column_namestr(),
                                       state_name, request_name, result_spec_name))) {
      LOG_WARN("fail to build hidden ai column names", K(ret));
    } else if (OB_ISNULL(state_col = table_schema.get_column_schema(state_name.ptr()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai state hidden column not found", K(ret), K(state_name));
    } else if (OB_ISNULL(request_col = table_schema.get_column_schema(request_name.ptr()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai request hidden column not found", K(ret), K(request_name));
    } else if (OB_ISNULL(result_spec_col = table_schema.get_column_schema(result_spec_name.ptr()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai result_spec hidden column not found", K(ret), K(result_spec_name));
    } else {
      ai_info.state_column_id_ = state_col->get_column_id();
      ai_info.request_column_id_ = request_col->get_column_id();
      ai_info.result_spec_column_id_ = result_spec_col->get_column_id();
      if (OB_UNLIKELY(!ai_info.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid ai column info", K(ret), K(ai_info));
      }
    }
  }
  return ret;
}

int ObAIColumnSqlService::add_ai_columns(common::ObISQLClient &sql_client,
                                         const ObTableSchema &table_schema,
                                         const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_ddl_allowed_(table_schema))) {
  } else {
    ObDMLSqlSplicer dml;
    int64_t row_count = 0;
    for (ObTableSchema::const_column_iterator iter = table_schema.column_begin();
        OB_SUCC(ret) && iter != table_schema.column_end(); ++iter) {
      if (OB_ISNULL(*iter)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const ObColumnSchemaV2 &column = **iter;
        if (!column.is_ai_column()) {
          // skip
        } else {
          ObAIColumnInfo ai_info;
          if (OB_FAIL(resolve_ai_column_info(table_schema, column, ai_info))) {
            LOG_WARN("fail to resolve ai column info", K(ret));
          } else if (OB_FAIL(gen_ai_column_dml(table_schema, column, ai_info, dml))) {
            LOG_WARN("fail to gen ai column dml", K(ret));
          } else {
            row_count++;
          }
        }
      }
    }
    if (OB_SUCC(ret) && row_count > 0) {
      if (OB_FAIL(exec_dml_(sql_client, OB_ALL_AI_COLUMN_TNAME, dml, row_count))) {
        LOG_WARN("fail to insert __all_ai_column", K(ret));
      } else if (OB_FAIL(set_history_defaults_(dml, schema_version))) {
        LOG_WARN("fail to set default columns", K(ret));
      } else if (OB_FAIL(exec_dml_(sql_client, OB_ALL_AI_COLUMN_HISTORY_TNAME, dml, row_count))) {
        LOG_WARN("fail to insert __all_ai_column_history", K(ret));
      }
    }
  }
  return ret;
}

int ObAIColumnSqlService::add_single_ai_column(common::ObISQLClient &sql_client,
                                               const ObTableSchema &table_schema,
                                               const ObColumnSchemaV2 &column,
                                               const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_ddl_allowed_(table_schema))) {
  } else if (OB_UNLIKELY(!column.is_ai_column())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObDMLSqlSplicer dml;
    ObAIColumnInfo ai_info;
    if (OB_FAIL(resolve_ai_column_info(table_schema, column, ai_info))) {
      LOG_WARN("fail to resolve ai column info", K(ret));
    } else if (OB_FAIL(gen_ai_column_dml(table_schema, column, ai_info, dml))) {
      LOG_WARN("fail to gen ai column dml", K(ret));
    } else if (OB_FAIL(exec_dml_(sql_client, OB_ALL_AI_COLUMN_TNAME, dml, 1))) {
      LOG_WARN("fail to insert __all_ai_column", K(ret));
    } else if (OB_FAIL(set_history_defaults_(dml, schema_version))) {
    } else if (OB_FAIL(exec_dml_(sql_client, OB_ALL_AI_COLUMN_HISTORY_TNAME, dml, 1))) {
      LOG_WARN("fail to insert __all_ai_column_history", K(ret));
    }
  }
  return ret;
}

int ObAIColumnSqlService::drop_ai_columns(common::ObISQLClient &sql_client,
                                          const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu",
                             OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column meta rows", K(ret), K(table_id));
  }
  if (OB_SUCC(ret)) {
    sql.reset();
    affected_rows = 0;
    if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu",
                               OB_ALL_AI_COLUMN_HISTORY_TNAME, table_id))) {
    } else if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
      LOG_WARN("fail to delete ai column meta history rows", K(ret), K(table_id));
    }
  }
  if (OB_SUCC(ret)) {
    sql.reset();
    affected_rows = 0;
    if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu",
                               OB_ALL_AI_COLUMN_TASK_TNAME, table_id))) {
    } else if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
      LOG_WARN("fail to delete ai column task rows", K(ret), K(table_id));
    }
  }
  return ret;
}

int ObAIColumnSqlService::set_history_defaults_(share::ObDMLSqlSplicer &dml, const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  ObSqlString schema_version_str;
  if (OB_FAIL(schema_version_str.assign_fmt("%ld", schema_version))) {
  } else if (OB_FAIL(dml.set_default_columns("is_deleted", "0"))) {
  } else if (OB_FAIL(dml.set_default_columns("schema_version", schema_version_str.ptr()))) {
  }
  return ret;
}

int ObAIColumnSqlService::check_ddl_allowed_(const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  if (!table_schema.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table schema", K(ret));
  }
  return ret;
}

int ObAIColumnSqlService::exec_dml_(common::ObISQLClient &sql_client,
                                    const char *table_name,
                                    const ObDMLSqlSplicer &dml,
                                    const int64_t target_affected_row_count)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (dml.empty()) {
    if (target_affected_row_count > 0) {
      ret = OB_ERR_UNEXPECTED;
    }
  } else {
    ObDMLExecHelper exec(sql_client);
    int64_t affected_rows = 0;
    if (OB_FAIL(exec.exec_batch_insert(table_name, dml, affected_rows))) {
      LOG_WARN("fail to exec batch insert", K(ret), K(table_name));
    } else if (target_affected_row_count >= 0 && affected_rows != target_affected_row_count) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected affected rows", K(ret), K(affected_rows), K(target_affected_row_count));
    }
  }
  return ret;
}

} // end namespace schema
} // end namespace share
} // end namespace oceanbase
