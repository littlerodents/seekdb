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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SQL_SERVICE_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SQL_SERVICE_H_

#include "share/ob_dml_sql_splicer.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/schema/ob_ai_column_info.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

class ObTableSchema;
class ObColumnSchemaV2;

// Persists AI column metadata into oceanbase.__all_ai_column (+history) within DDL
// transactions, and cleans task/meta rows when the owning table goes away.
class ObAIColumnSqlService
{
public:
  // Canonical hidden column names backing an AI column `column_name`.
  static int hidden_ai_column_names(const common::ObString &column_name,
                                    common::ObSqlString &state_name,
                                    common::ObSqlString &request_name,
                                    common::ObSqlString &result_spec_name);
  // Fill hidden column ids by looking up names in the assembled table schema.
  static int resolve_ai_column_info(const ObTableSchema &table_schema,
                                    const ObColumnSchemaV2 &column,
                                    ObAIColumnInfo &ai_info);
  // Insert one __all_ai_column row (+history) for every AI column of the table.
  static int add_ai_columns(common::ObISQLClient &sql_client,
                            const ObTableSchema &table_schema,
                            const int64_t schema_version);
  // Insert for a single (alter-added) AI column.
  static int add_single_ai_column(common::ObISQLClient &sql_client,
                                  const ObTableSchema &table_schema,
                                  const ObColumnSchemaV2 &column,
                                  const int64_t schema_version);
  // Delete all meta/history/task rows of a table (drop table / drop database).
  static int drop_ai_columns(common::ObISQLClient &sql_client,
                             const uint64_t table_id);
private:
  static int check_ddl_allowed_(const ObTableSchema &table_schema);
  static int exec_dml_(common::ObISQLClient &sql_client,
                       const char *table_name,
                       const share::ObDMLSqlSplicer &dml,
                       const int64_t target_affected_row_count);
  // __all_ai_column and __all_ai_column_history disagree on their column sets:
  // only the history table carries schema_version and is_deleted, so one splicer
  // cannot serve both.
  static int gen_ai_column_dml(const ObTableSchema &table_schema,
                               const ObColumnSchemaV2 &column,
                               const ObAIColumnInfo &ai_info,
                               const bool for_history,
                               const int64_t schema_version,
                               share::ObDMLSqlSplicer &dml);
};

} // end namespace schema
} // end namespace share
} // end namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SQL_SERVICE_H_
