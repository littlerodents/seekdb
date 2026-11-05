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

#ifndef OCEANBASE_ROOTSERVER_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_ROOTSERVER_OB_AI_COLUMN_WORKER_H_

#include "lib/task/ob_timer.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_se_array.h"
#include "common/mysqlclient/ob_mysql_proxy.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObMultiVersionSchemaService;
class ObTableSchema;
struct ObAIColumnInfo;
}
}
namespace rootserver
{

// Background worker that drains oceanbase.__all_ai_column_task:
//   - groups eligible tasks by (table, rowkey)
//   - reads the current request value of every AI column of the row
//   - calls the configured AI model endpoint (completion with a type-specific
//     system prompt, or a dense embedding request)
//   - writes all results of one row back atomically, guarded by the request
//     column so a superseded request can never overwrite a newer result
//   - applies the configured retry policy and finally records FAILED state
class ObAIColumnWorker
{
public:
  ObAIColumnWorker();
  ~ObAIColumnWorker();
  int init(common::ObMySQLProxy *sql_proxy,
           share::schema::ObMultiVersionSchemaService *schema_service);
  int start();
  void stop();
  void process_once();

private:
  struct TaskItem
  {
    TaskItem() : table_id_(0), column_id_(0), retry_count_(0), max_retries_(0) {}
    uint64_t table_id_;
    uint64_t column_id_;
    common::ObString rowkey_enc_;
    common::ObString rowkey_predicate_;
    common::ObString request_id_;
    common::ObString request_snapshot_;
    int64_t retry_count_;
    int64_t max_retries_;
    common::ObString ai_model_name_;
    common::ObString ai_func_;
    common::ObString normalized_expr_;
  };

  class ObAIColumnWorkerTask : public common::ObTimerTask
  {
  public:
    explicit ObAIColumnWorkerTask(ObAIColumnWorker &worker) : worker_(worker) {}
    virtual ~ObAIColumnWorkerTask() = default;
    virtual void runTimerTask() override;
  private:
    ObAIColumnWorker &worker_;
  };

private:
  int fetch_eligible_tasks(common::ObIAllocator &alloc, common::ObIArray<TaskItem> &tasks);
  int process_group(common::ObIAllocator &alloc,
                    const share::schema::ObTableSchema &table_schema,
                    const common::ObIArray<TaskItem> &group);
  int delete_task_rows(const common::ObIArray<TaskItem> &group);
  bool inited_;
  bool started_;
  common::ObMySQLProxy *sql_proxy_;
  share::schema::ObMultiVersionSchemaService *schema_service_;
  common::ObTimer timer_;
  ObAIColumnWorkerTask worker_task_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObAIColumnWorker);
};

} // namespace rootserver
} // namespace oceanbase

#endif // OCEANBASE_ROOTSERVER_OB_AI_COLUMN_WORKER_H_
