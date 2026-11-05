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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_INFO_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_INFO_H_

#include "lib/ob_name_def.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// Per-AI-column metadata persisted in oceanbase.__all_ai_column and cached in ObTableSchema.
// An AI column `c` of type T declared as
//   `c` T AI COLUMN (AI_COMPLETE('model', <input expr>)) WITH (sync_mode='async', max_retries=2)
// is physically backed by three hidden columns:
//   __ai_<c>_state        bigint,   0=PENDING 2=READY 4=FAILED (kept by retry DDL/worker)
//   __ai_<c>_request      varchar,  the evaluated <input expr> of the latest write
//   __ai_<c>_result_spec  varchar,  the request value that produced the current result
// the user-visible result column `c` itself holds the model output when ready and is
// nulled synchronously by DML on dependency change.
struct ObAIColumnInfo
{
  OB_UNIS_VERSION(1);

public:
  ObAIColumnInfo() : ObAIColumnInfo(nullptr) {}
  explicit ObAIColumnInfo(common::ObIAllocator *allocator)
    : table_id_(common::OB_INVALID_ID),
      column_id_(common::OB_INVALID_ID),
      state_column_id_(common::OB_INVALID_ID),
      request_column_id_(common::OB_INVALID_ID),
      result_spec_column_id_(common::OB_INVALID_ID),
      column_name_(),
      normalized_expr_(),
      type_prompt_(),
      ai_model_name_(),
      ai_func_(),
      sync_mode_(),
      max_retries_(2),
      embed_dim_(-1)
  {
  }
  ~ObAIColumnInfo() {}
  void reset()
  {
    table_id_ = common::OB_INVALID_ID;
    column_id_ = common::OB_INVALID_ID;
    state_column_id_ = common::OB_INVALID_ID;
    request_column_id_ = common::OB_INVALID_ID;
    result_spec_column_id_ = common::OB_INVALID_ID;
    column_name_.reset();
    normalized_expr_.reset();
    type_prompt_.reset();
    ai_model_name_.reset();
    ai_func_.reset();
    sync_mode_.reset();
    max_retries_ = 2;
    embed_dim_ = -1;
  }
  bool is_valid() const
  {
    return common::OB_INVALID_ID != table_id_
        && common::OB_INVALID_ID != column_id_
        && common::OB_INVALID_ID != state_column_id_
        && common::OB_INVALID_ID != request_column_id_
        && common::OB_INVALID_ID != result_spec_column_id_
        && !column_name_.empty()
        && !ai_model_name_.empty()
        && (!ai_func_.empty());
  }
  bool operator==(const ObAIColumnInfo &other) const
  {
    return table_id_ == other.table_id_
        && column_id_ == other.column_id_
        && state_column_id_ == other.state_column_id_
        && request_column_id_ == other.request_column_id_
        && result_spec_column_id_ == other.result_spec_column_id_
        && column_name_ == other.column_name_
        && normalized_expr_ == other.normalized_expr_
        && type_prompt_ == other.type_prompt_
        && ai_model_name_ == other.ai_model_name_
        && ai_func_ == other.ai_func_
        && sync_mode_ == other.sync_mode_
        && max_retries_ == other.max_retries_
        && embed_dim_ == other.embed_dim_;
  }
  TO_STRING_KV(K_(table_id),
               K_(column_id),
               K_(state_column_id),
               K_(request_column_id),
               K_(result_spec_column_id),
               K_(column_name),
               K_(normalized_expr),
               K_(type_prompt),
               K_(ai_model_name),
               K_(ai_func),
               K_(sync_mode),
               K_(max_retries),
               K_(embed_dim));

  uint64_t table_id_;
  uint64_t get_table_id() const { return table_id_; }
  uint64_t &set_table_id() { return table_id_; }
  uint64_t &set_column_id() { return column_id_; }
  uint64_t &set_state_column_id() { return state_column_id_; }
  uint64_t &set_request_column_id() { return request_column_id_; }
  uint64_t &set_result_spec_column_id() { return result_spec_column_id_; }
  int64_t &set_max_retries() { return max_retries_; }
  int64_t &set_embed_dim() { return embed_dim_; }
  int set_column_name(const common::ObString &v) { column_name_ = v; return 0; }
  int set_normalized_expr(const common::ObString &v) { normalized_expr_ = v; return 0; }
  int set_type_prompt(const common::ObString &v) { type_prompt_ = v; return 0; }
  int set_ai_model_name(const common::ObString &v) { ai_model_name_ = v; return 0; }
  int set_ai_func(const common::ObString &v) { ai_func_ = v; return 0; }
  int set_sync_mode(const common::ObString &v) { sync_mode_ = v; return 0; }
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  common::ObString column_name_;
  // SQL text of the input expression; may carry a trailing /* type prompt */ comment.
  common::ObString normalized_expr_;
  // system prompt derived from the column type at DDL time (sent by the async worker)
  common::ObString type_prompt_;
  common::ObString ai_model_name_;
  // "complete" or "embed"
  common::ObString ai_func_;
  common::ObString sync_mode_;
  int64_t max_retries_;
  int64_t embed_dim_;
};

} // end namespace schema
} // end namespace share
} // end namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_INFO_H_
