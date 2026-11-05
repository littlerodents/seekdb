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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_VALUES_TABLE_ACCESS_OP_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_VALUES_TABLE_ACCESS_OP_

#include "sql/engine/ob_operator.h"
#include "sql/engine/expr/ob_datum_cast.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "lib/allocator/page_arena.h"

namespace oceanbase
{
namespace sql
{
class ObPhyOpSeriCtx;

class ObValuesTableAccessSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObValuesTableAccessSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), access_type_(ObValuesTableDef::ACCESS_EXPR), column_exprs_(alloc),
      value_exprs_(alloc), start_param_idx_(-1), end_param_idx_(-1), obj_params_(alloc),
      file_kind_(-1), file_header_(true), file_delim_(','), file_quote_('"'), file_escape_('"'),
      file_skip_(0), file_identity_(alloc), file_column_indexes_(alloc) {}
  INHERIT_TO_STRING_KV("op_spec", ObOpSpec, K_(access_type));
  ObValuesTableDef::TableAccessType access_type_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> value_exprs_;
  int64_t start_param_idx_;
  int64_t end_param_idx_;
  common::ObFixedArray<ObObj, common::ObIAllocator> obj_params_;
  common::ObString file_path_;
  common::ObString file_nullstr_;
  int64_t file_kind_;
  bool file_header_;
  int64_t file_delim_;
  int64_t file_quote_;
  int64_t file_escape_;
  int64_t file_skip_;
  common::ObFixedArray<int64_t, common::ObIAllocator> file_identity_;
  common::ObFixedArray<int64_t, common::ObIAllocator> file_column_indexes_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObValuesTableAccessSpec);
};

class ObValuesTableAccessOp : public ObOperator
{
public:
  ObValuesTableAccessOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input);
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int switch_iterator() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int open_file_reader();
  void close_file_reader();
  int read_file_row();
  int calc_next_row();
  int calc_datum_from_expr(const int64_t col_idx,
                           const int64_t row_idx,
                           ObExpr *src_expr,
                           ObExpr *dst_expr);
  int calc_datum_from_param(const ObObj &src_obj, ObExpr *dst_expr);
  void update_src_meta(const ObObjMeta &src_obj_meta,
                       const ObAccuracy &src_obj_acc,
                       ObDatumMeta &src_meta);
  int get_real_src_obj_type(const int64_t row_idx,
                            ObExpr &src_expr,
                            ObDatumMeta &src_meta,
                            ObObjMeta &src_obj_meta);
private:
  DISALLOW_COPY_AND_ASSIGN(ObValuesTableAccessOp);

private:
  ObDatumCaster datum_caster_;
  common::ObCastMode cm_;
  int64_t row_idx_;
  int64_t row_cnt_;
  FileCsvStream *csv_reader_;
  FileJsonlStream *jsonl_reader_;
  FileParquetStream *parquet_reader_;
  common::ObArenaAllocator file_row_alloc_;
  common::ObSEArray<common::ObObj, 16> file_row_;
};

} // end namespace sql
} // end namespace oceanbase
#endif
