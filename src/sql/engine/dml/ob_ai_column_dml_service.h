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

#ifndef OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_DML_SERVICE_H_
#define OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_DML_SERVICE_H_

#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_se_array.h"
#include "common/object/ob_object.h"

namespace oceanbase
{

namespace share
{
namespace schema
{
class ObTableSchema;
class ObSchemaGetterGuard;
class ObAIColumnInfo;
}
}

namespace sql
{

class ObExecContext;
class ObExpr;
class ObEvalCtx;

// Runtime maintenance of AI columns during user DML. All work is done through
// nested SQL executed on the user's own session (same transaction), so task
// rows and row invalidation are visible inside the user transaction and rolled
// back with it.
//
// Physical model of an AI column `c` with input expression E(c-dependent cols):
//   `c`                      user column, holds the model result when ready
//   `__ai_c_state`           0=PENDING 2=READY 4=FAILED
//   `__ai_c_request`         latest evaluated E
//   `__ai_c_result_spec`     the request value that produced the current result
// `c` reads NULL while `__ai_c_result_spec` does not equal `__ai_c_request`.
class ObAIColumnDMLService
{
public:
  // ---------- SQL text helpers (also used by the async worker) ----------
  static int escape_string(const common::ObString &src, common::ObSqlString &dst);
  static int obj_to_sql_literal(const common::ObObj &obj, common::ObSqlString &dst);

  // ---------- DML op hooks ----------
  // Called once when the write op opens: activates the hook when the table has AI columns.
  static int on_write_op_open(ObExecContext &ctx,
                              const uint64_t table_id,
                              common::ObIArray<common::ObString> &pk_names,
                              bool &hook_active);
  // Called per written row: format and buffer the rowkey predicate/encoding of the row.
  static int collect_written_rowkey(common::ObIAllocator &alloc,
                                    ObEvalCtx &eval_ctx,
                                    const common::ObIArray<common::ObString> &pk_names,
                                    const common::ObIArray<ObExpr *> &full_row,
                                    const int64_t rowkey_cnt,
                                    common::ObIArray<common::ObString> &rowkey_preds,
                                    common::ObIArray<common::ObString> &rowkey_encs);
  // Build the (predicate, encoding) pair for a single column rowkey from an already
  // materialised value.  Used when an update moves the row to another primary key: the
  // value the statement writes is available as a datum, but re-evaluating the plan's
  // expressions at this point corrupts the affected row accounting of the statement.
  static int build_single_rowkey(common::ObIAllocator &alloc,
                                 const common::ObString &pk_name,
                                 const common::ObObj &obj,
                                 common::ObIArray<common::ObString> &rowkey_preds,
                                 common::ObIArray<common::ObString> &rowkey_encs);

  // True when the update writes at least one primary key column, i.e. the row moves to a
  // different identity, so any result it carries over has to be invalidated even though the
  // dependency expression is unchanged.
  static int writes_rowkey(ObExecContext &ctx,
                           const uint64_t table_id,
                           const common::ObIArray<uint64_t> &written_column_ids,
                           bool &writes_pk,
                           uint64_t &written_rowkey_id);

  // Called at statement end: create/refresh tasks and invalidate results (insert/update)
  // or drop tasks (delete). All nested SQL joins the user transaction.
  static int finish_write(ObExecContext &ctx,
                          const uint64_t table_id,
                          const bool is_delete_op,
                          const bool is_insert_op,
                          const common::ObIArray<common::ObString> &rowkey_preds,
                          const common::ObIArray<common::ObString> &rowkey_encs,
                          const bool force_invalidate = false);

  // ---------- RETRY AI COLUMN ----------
  static int execute_ai_column_retry(ObExecContext &ctx,
                                     const uint64_t table_id,
                                     const common::ObString &column_name,
                                     const common::ObString &predicate);

  // ---------- strict read ----------
  // Returns OB_AI_COLUMN_UNFINISHED_TASK when the table has rows whose AI state
  // is not READY for any referenced AI column.
  static int check_strict_read(ObExecContext &ctx,
                               const common::ObIArray<uint64_t> &table_ids,
                               const common::ObIArray<common::ObString> &column_names);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_DML_SERVICE_H_
