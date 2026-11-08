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

#define USING_LOG_PREFIX RS

#include "lib/oblog/ob_log.h"
#include "lib/oblog/ob_log_module.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "rootserver/ob_ai_column_worker.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/time/ob_time_utility.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_ai_column_sql_service.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "share/ai_service/ob_ai_service_struct.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace rootserver
{

namespace ob_ai_worker
{

const int64_t POLL_INTERVAL_US = 200 * 1000;          // 200ms
const int64_t MAX_TASKS_PER_ROUND = 64;
const int64_t RETRY_BACKOFF_SECONDS = 1;

const char *JSON_TYPE_PROMPT =
    "Return exactly one valid JSON value. Respond with only the JSON value itself, "
    "without explanations, markdown or surrounding text.";
const char *INTEGER_TYPE_PROMPT =
    "Return exactly one base-10 integer. Respond with only the integer itself, "
    "without explanations or surrounding text.";
const char *DEFAULT_TYPE_PROMPT =
    "You are a helpful assistant.";

int escape_string(const ObString &src, ObSqlString &dst)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < src.length(); ++i) {
    const char c = src.ptr()[i];
    if ('\'' == c) {
      ret = dst.append("''");
    } else if ('\\' == c) {
      ret = dst.append("\\\\");
    } else {
      ret = dst.append(&c, 1);
    }
  }
  return ret;
}

int write_quoted(const ObString &src, ObSqlString &dst)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(dst.append("\"", 1))) {
  } else if (OB_FAIL(escape_string(src, dst))) {
  } else if (OB_FAIL(dst.append("\"", 1))) {
  }
  return ret;
}

// The AI function utilities print JSON string nodes with surrounding quotes;
// SQL literals must not carry them.
int strip_json_quotes(const ObString &src, ObString &dst)
{
  dst = src;
  if (src.length() >= 2 && '"' == src[0] && '"' == src[src.length() - 1]) {
    dst = ObString(src.length() - 2, src.ptr() + 1);
  }
  return OB_SUCCESS;
}

const char *type_prompt_for(const ObColumnSchemaV2 *column)
{
  if (OB_ISNULL(column)) {
    return DEFAULT_TYPE_PROMPT;
  } else if (column->is_json()) {
    return JSON_TYPE_PROMPT;
  } else if (column->get_meta_type().is_integer_type()) {
    return INTEGER_TYPE_PROMPT;
  } else {
    return DEFAULT_TYPE_PROMPT;
  }
}

} // end namespace ob_ai_worker

ObAIColumnWorker::ObAIColumnWorker()
    : inited_(false), started_(false), sql_proxy_(NULL), schema_service_(NULL),
      timer_(), worker_task_(*this)
{
}

ObAIColumnWorker::~ObAIColumnWorker()
{
  stop();
}

int ObAIColumnWorker::init(common::ObMySQLProxy *sql_proxy,
                           share::schema::ObMultiVersionSchemaService *schema_service)
{
  int ret = OB_SUCCESS;
  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(sql_proxy) || OB_ISNULL(schema_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(timer_.init("AIColWorker", ObMemAttr("AIColWorker")))) {
    LOG_WARN("fail to init ai column worker timer", K(ret));
  } else {
    sql_proxy_ = sql_proxy;
    schema_service_ = schema_service;
    inited_ = true;
  }
  return ret;
}

void ObAIColumnWorker::ObAIColumnWorkerTask::runTimerTask()
{
  worker_.process_once();
}

int ObAIColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (started_) {
    // already started
  } else if (OB_FAIL(timer_.schedule(worker_task_, ob_ai_worker::POLL_INTERVAL_US,
                                      true /*repeat*/, true /*immediately*/))) {
    LOG_WARN("fail to schedule ai column worker task", K(ret));
  } else {
    started_ = true;
    LOG_INFO("ai column worker started");
  }
  return ret;
}

void ObAIColumnWorker::stop()
{
  if (started_) {
    timer_.cancel_task(worker_task_);
    started_ = false;
  }
  timer_.destroy();
}

int ObAIColumnWorker::fetch_eligible_tasks(common::ObIAllocator &alloc,
                                           common::ObIArray<TaskItem> &tasks)
{
  int ret = OB_SUCCESS;
  const char *sql =
      "SELECT t.table_id, t.column_id, t.rowkey_enc, t.rowkey_predicate, t.request_id, "
      "t.retry_count, t.request, a.ai_model_name, a.ai_func, a.normalized_expr, a.max_retries "
      "FROM oceanbase.__all_ai_column_task t JOIN oceanbase.__all_ai_column a "
      "ON a.table_id = t.table_id AND a.column_id = t.column_id "
      "WHERE t.state IN (0, 3) AND t.eligible_ts <= CURRENT_TIMESTAMP(6) "
      "ORDER BY t.table_id, t.rowkey_enc "
      "LIMIT %ld";
  SMART_VAR(ObMySQLProxy::MySQLResult, res)
  {
    common::sqlclient::ObMySQLResult *result = NULL;
    ObSqlString real_sql;
    if (OB_FAIL(real_sql.assign_fmt(sql, ob_ai_worker::MAX_TASKS_PER_ROUND))) {
    } else if (OB_FAIL(sql_proxy_->read(res, real_sql.ptr()))) {
      LOG_WARN("fail to read eligible ai column tasks", K(ret), K(real_sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      while (OB_SUCC(ret) && OB_SUCCESS == (ret = result->next())) {
        TaskItem item;
        ObString tmp;
        int64_t tmp_int = 0;
        if (OB_FAIL(result->get_int("table_id", tmp_int))) {
        } else {
          item.table_id_ = static_cast<uint64_t>(tmp_int);
          if (OB_FAIL(result->get_int("column_id", tmp_int))) {
          } else {
            item.column_id_ = static_cast<uint64_t>(tmp_int);
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(result->get_varchar("rowkey_enc", tmp))) {
        } else if (OB_FAIL(ob_write_string(alloc, tmp, item.rowkey_enc_))) {
        } else if (OB_FAIL(result->get_varchar("rowkey_predicate", tmp))) {
        } else if (OB_FAIL(ob_write_string(alloc, tmp, item.rowkey_predicate_))) {
        } else if (OB_FAIL(result->get_varchar("request_id", tmp))) {
        } else if (OB_FAIL(ob_write_string(alloc, tmp, item.request_id_))) {
        } else if (OB_FAIL(result->get_int("retry_count", item.retry_count_))) {
        } else if (OB_FAIL(result->get_int("max_retries", item.max_retries_))) {
        }
        // nullable columns
        if (OB_SUCC(ret)) {
          if (OB_FAIL(result->get_varchar("request", tmp))) {
            if (OB_ERR_NULL_VALUE == ret || OB_ERR_COLUMN_NOT_FOUND == ret) {
              ret = OB_SUCCESS;
              item.request_snapshot_.reset();
            }
          } else if (OB_FAIL(ob_write_string(alloc, tmp, item.request_snapshot_))) {
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(result->get_varchar("ai_model_name", tmp))) {
          } else if (OB_FAIL(ob_write_string(alloc, tmp, item.ai_model_name_))) {
          } else if (OB_FAIL(result->get_varchar("ai_func", tmp))) {
          } else if (OB_FAIL(ob_write_string(alloc, tmp, item.ai_func_))) {
          } else if (OB_FAIL(result->get_varchar("normalized_expr", tmp))) {
          } else if (OB_FAIL(ob_write_string(alloc, tmp, item.normalized_expr_))) {
          } else if (OB_FAIL(tasks.push_back(item))) {
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      } else if (OB_SUCCESS != ret) {
        LOG_WARN("fail to iterate ai column tasks", K(ret));
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::delete_task_rows(const common::ObIArray<TaskItem> &group)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const TaskItem &item = group.at(i);
    ObSqlString sql;
    if (OB_FAIL(sql.assign_fmt(
            "DELETE FROM oceanbase.__all_ai_column_task "
            "WHERE table_id = %lu AND column_id = %lu AND rowkey_enc = '%s' AND request_id = '%s'",
            item.table_id_, item.column_id_, item.rowkey_enc_.ptr(), item.request_id_.ptr()))) {
    } else if (OB_FAIL(sql_proxy_->write(sql.ptr(), affected))) {
      LOG_WARN("fail to delete ai column task row", K(ret), K(sql));
    }
  }
  return ret;
}

int ObAIColumnWorker::process_group(common::ObIAllocator &alloc,
                                    const ObTableSchema &table_schema,
                                    const common::ObIArray<TaskItem> &group)
{
  int ret = OB_SUCCESS;
  const ObString &tb_name = table_schema.get_table_name_str();
  share::schema::ObSchemaGetterGuard db_guard;
  const ObString *db_name_ptr = NULL;
  ObString db_name;
  if (OB_FAIL(schema_service_->get_runtime_schema_guard(db_guard))) {
    LOG_WARN("fail to get schema guard for db name", K(ret));
  } else if (OB_FAIL(db_guard.get_database_name(table_schema.get_database_id(), db_name_ptr))) {
    LOG_WARN("fail to get database name", K(ret), "db_id", table_schema.get_database_id());
  } else if (OB_ISNULL(db_name_ptr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null database name", K(ret), "db_id", table_schema.get_database_id());
  } else {
    db_name = *db_name_ptr;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(group.empty() || db_name.empty() || tb_name.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ObString &pred = group.at(0).rowkey_predicate_;
    // 1) read the current request values of every column in the group
    ObSqlString read_sql;
    ObSEArray<ObString, 4> request_cols;
    ObSEArray<const ObAIColumnInfo *, 4> infos;
    ObSEArray<ObString, 4> current_requests;
    ObSEArray<const ObColumnSchemaV2 *, 4> columns;
    if (OB_FAIL(read_sql.assign_fmt("SELECT "))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      const ObAIColumnInfo *info = table_schema.get_ai_column_by_column_id(group.at(i).column_id_);
      if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column info missing in schema", K(ret), "column_id", group.at(i).column_id_);
      } else {
        if (i > 0 && OB_FAIL(read_sql.append(", "))) {
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(read_sql.append_fmt("__ai_%.*s_request",
                                                static_cast<int>(info->column_name_.length()),
                                                info->column_name_.ptr()))) {
        } else if (OB_FAIL(infos.push_back(info))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(read_sql.append_fmt(
            " FROM `%.*s`.`%.*s` WHERE %s",
            static_cast<int>(db_name.length()), db_name.ptr(),
            static_cast<int>(tb_name.length()), tb_name.ptr(), pred.ptr()))) {
    } else {
      bool row_exists = false;
      SMART_VAR(ObMySQLProxy::MySQLResult, res)
      {
        common::sqlclient::ObMySQLResult *result = NULL;
        if (OB_FAIL(sql_proxy_->read(res, read_sql.ptr()))) {
          LOG_WARN("fail to read current ai requests", K(ret), K(read_sql));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ITER_END == (ret = result->next())) {
          // the row disappeared (deleted or pk-updated); drop the tasks
          ret = OB_SUCCESS;
        } else if (OB_SUCC(ret)) {
          row_exists = true;
          for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
            ObString request_value;
            if (OB_FAIL(result->get_varchar(0 + i, request_value))) {
              if (OB_ERR_NULL_VALUE == ret) {
                ret = OB_SUCCESS;
                request_value.reset();
              } else {
                LOG_WARN("fail to read request value", K(ret));
              }
            }
            if (OB_SUCC(ret)) {
              ObString saved;
              if (OB_FAIL(ob_write_string(alloc, request_value, saved))) {
              } else if (OB_FAIL(current_requests.push_back(saved))) {
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (!row_exists) {
          if (OB_FAIL(delete_task_rows(group))) {
          }
        } else {
          // 2) call the model for every column with a pending task whose request is ready
          ObSEArray<ObString, 4> results;
          ObSEArray<bool, 4> succeeded;
          int64_t succeeded_cnt = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
            const ObAIColumnInfo *info = infos.at(i);
            const ObColumnSchemaV2 *column = table_schema.get_column_schema(info->column_id_);
            ObString model_result;
            bool ok = false;
            if (current_requests.at(i).empty()) {
              // invalidation has not landed yet; retry later
            } else {
              common::ObAIFuncExprInfo *ai_func_info = NULL;
              query::ObIAiEndpointResolver *endpoint_resolver =
                  ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
              share::ObAiModelEndpointInfo endpoint_info;
              if (OB_FAIL(common::ObAIFuncUtils::get_ai_func_info(
                      alloc, group.at(i).ai_model_name_, ai_func_info))) {
                LOG_WARN("fail to resolve ai model info", K(ret), K(group.at(i).ai_model_name_));
              } else if (OB_ISNULL(endpoint_resolver)) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                      group.at(i).ai_model_name_, alloc, endpoint_info))) {
                LOG_WARN("fail to resolve ai endpoint", K(ret), K(group.at(i).ai_model_name_));
              } else {
                common::ObAIFuncModel model(alloc, *ai_func_info, endpoint_info);
                const bool is_embed = (0 == group.at(i).ai_func_.case_compare("embed"));
                if (is_embed) {
                  ObString request = current_requests.at(i);
                  if (OB_FAIL(model.call_dense_embedding(request, NULL, model_result))) {
                    LOG_WARN("ai column embedding call failed", K(ret));
                  } else {
                    ok = true;
                  }
                } else {
                  ObString request = current_requests.at(i);
                  ObString system_prompt(ob_ai_worker::type_prompt_for(column));
                  if (OB_FAIL(model.call_completion_with_system(request, system_prompt, NULL, model_result))) {
                    LOG_WARN("ai column completion call failed", K(ret));
                  } else {
                    ok = true;
                  }
                }
              }
              if (OB_FAIL(ret)) {
                // model-level failure: record and continue with other columns
                ret = OB_SUCCESS;
                ok = false;
              }
            }
            if (OB_FAIL(ret)) {
            } else {
              ObString cleaned;
              if (ok) {
                ob_ai_worker::strip_json_quotes(model_result, cleaned);
                ObString saved;
                if (OB_FAIL(ob_write_string(alloc, cleaned, saved))) {
                  ok = false;
                }
                model_result = OB_SUCC(ret) ? saved : model_result;
              }
              if (OB_FAIL(results.push_back(model_result))) {
              } else if (OB_FAIL(succeeded.push_back(ok))) {
              } else if (ok) {
                ++succeeded_cnt;
              }
            }
          }
          // 3) write back: only when every column of the group succeeded (atomic visibility)
          if (OB_SUCC(ret)) {
            if (succeeded_cnt == group.count() && succeeded_cnt > 0) {
              ObSqlString write_sql;
              if (OB_FAIL(write_sql.assign_fmt(
                      "UPDATE `%.*s`.`%.*s` SET ",
                      static_cast<int>(db_name.length()), db_name.ptr(),
                      static_cast<int>(tb_name.length()), tb_name.ptr()))) {
              }
              for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
                const ObAIColumnInfo *info = infos.at(i);
                if (i > 0 && OB_FAIL(write_sql.append(", "))) {
                }
                if (OB_FAIL(ret)) {
                } else if (OB_FAIL(write_sql.append_fmt(
                        "`%.*s` = ",
                        static_cast<int>(info->column_name_.length()), info->column_name_.ptr()))) {
                } else if (OB_FAIL(ob_ai_worker::write_quoted(results.at(i), write_sql))) {
                } else if (OB_FAIL(write_sql.append_fmt(
                        ", __ai_%.*s_state = 2",
                        static_cast<int>(info->column_name_.length()), info->column_name_.ptr()))) {
                } else if (OB_FAIL(write_sql.append_fmt(
                        ", __ai_%.*s_result_spec = ",
                        static_cast<int>(info->column_name_.length()), info->column_name_.ptr()))) {
                } else if (OB_FAIL(ob_ai_worker::write_quoted(current_requests.at(i), write_sql))) {
                }
              }
              if (OB_SUCC(ret)) {
                if (OB_FAIL(write_sql.append_fmt(" WHERE %s", pred.ptr()))) {
                }
                for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
                  const ObAIColumnInfo *info = infos.at(i);
                  if (OB_FAIL(write_sql.append_fmt(
                          " AND __ai_%.*s_request = ",
                          static_cast<int>(info->column_name_.length()), info->column_name_.ptr()))) {
                  } else if (OB_FAIL(ob_ai_worker::write_quoted(current_requests.at(i), write_sql))) {
                  }
                }
              }
              int64_t affected = 0;
              if (OB_SUCC(ret)) {
                if (OB_FAIL(sql_proxy_->write(write_sql.ptr(), affected))) {
                  LOG_WARN("fail to write back ai results", K(ret), K(write_sql));
                }
              }
              if (OB_SUCC(ret)) {
                if (OB_FAIL(delete_task_rows(group))) {
                }
              }
            } else {
              // per-column retry / terminal failure handling
              for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
                const TaskItem &item = group.at(i);
                const ObAIColumnInfo *info = infos.at(i);
                int64_t affected = 0;
                ObSqlString sql;
                if (succeeded.at(i)) {
                  // leave the task pending; retry next round until the whole group succeeds
                } else if (item.retry_count_ < item.max_retries_) {
                  if (OB_FAIL(sql.assign_fmt(
                          "UPDATE oceanbase.__all_ai_column_task SET state = 3, "
                          "retry_count = retry_count + 1, "
                          "eligible_ts = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL %ld SECOND) "
                          "WHERE table_id = %lu AND column_id = %lu AND rowkey_enc = '%s' "
                          "AND request_id = '%s'",
                          ob_ai_worker::RETRY_BACKOFF_SECONDS,
                          item.table_id_, item.column_id_, item.rowkey_enc_.ptr(),
                          item.request_id_.ptr()))) {
                  } else if (OB_FAIL(sql_proxy_->write(sql.ptr(), affected))) {
                    LOG_WARN("fail to schedule ai task retry", K(ret), K(sql));
                  }
                } else {
                  // terminal failure: mark the row FAILED and drop the task
                  ObSqlString fail_sql;
                  if (OB_FAIL(fail_sql.assign_fmt(
                          "UPDATE `%.*s`.`%.*s` SET __ai_%.*s_state = 4 WHERE %s "
                          "AND __ai_%.*s_request <=> ",
                          static_cast<int>(db_name.length()), db_name.ptr(),
                          static_cast<int>(tb_name.length()), tb_name.ptr(),
                          static_cast<int>(info->column_name_.length()), info->column_name_.ptr(),
                          pred.ptr(),
                          static_cast<int>(info->column_name_.length()), info->column_name_.ptr()))) {
                  } else if (OB_FAIL(ob_ai_worker::write_quoted(current_requests.at(i), fail_sql))) {
                  } else if (OB_FAIL(sql_proxy_->write(fail_sql.ptr(), affected))) {
                    LOG_WARN("fail to mark ai column failed", K(ret), K(fail_sql));
                  } else {
                    ObSqlString del_sql;
                    if (OB_FAIL(del_sql.assign_fmt(
                            "DELETE FROM oceanbase.__all_ai_column_task "
                            "WHERE table_id = %lu AND column_id = %lu AND rowkey_enc = '%s' "
                            "AND request_id = '%s'",
                            item.table_id_, item.column_id_, item.rowkey_enc_.ptr(),
                            item.request_id_.ptr()))) {
                    } else if (OB_FAIL(sql_proxy_->write(del_sql.ptr(), affected))) {
                      LOG_WARN("fail to delete failed ai task", K(ret), K(del_sql));
                    }
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

void ObAIColumnWorker::process_once()
{
  int ret = OB_SUCCESS;
  if (!inited_ || OB_ISNULL(sql_proxy_) || OB_ISNULL(schema_service_)) {
  } else {
    ObArenaAllocator alloc(ObModIds::OB_TEMP_VARIABLES);
    ObSEArray<TaskItem, 16> tasks;
    if (OB_FAIL(fetch_eligible_tasks(alloc, tasks))) {
      LOG_WARN("fail to fetch ai column tasks", K(ret));
    } else if (tasks.empty()) {
      // idle
    } else {
      // group consecutive tasks of the same (table_id, rowkey_enc)
      int64_t begin = 0;
      while (OB_SUCC(ret) && begin < tasks.count()) {
        int64_t end = begin + 1;
        while (end < tasks.count()
               && tasks.at(end).table_id_ == tasks.at(begin).table_id_
               && 0 == tasks.at(end).rowkey_enc_.compare(tasks.at(begin).rowkey_enc_)) {
          ++end;
        }
        share::schema::ObSchemaGetterGuard guard;
        const ObTableSchema *table_schema = NULL;
        ObSEArray<TaskItem, 4> group;
        for (int64_t i = begin; i < end; ++i) {
          if (OB_FAIL(group.push_back(tasks.at(i)))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(schema_service_->get_runtime_schema_guard(guard))) {
          LOG_WARN("fail to get schema guard", K(ret));
        } else if (OB_FAIL(guard.get_table_schema(group.at(0).table_id_, table_schema))) {
          LOG_WARN("fail to get table schema", K(ret));
        } else if (OB_ISNULL(table_schema)) {
          // table dropped; clean the tasks
          if (OB_FAIL(delete_task_rows(group))) {
          }
        } else if (OB_FAIL(process_group(alloc, *table_schema, group))) {
          LOG_WARN("fail to process ai column group", K(ret));
          ret = OB_SUCCESS; // continue with other groups
        }
        begin = end;
      }
    }
  }
}

} // namespace rootserver
} // namespace oceanbase
