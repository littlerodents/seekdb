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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_content->is_null()) {
    res.set_null();
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->is_null() ? 0 : arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::read_string_datum(const ObExpr &arg_expr, ObEvalCtx &ctx,
                                     ObIAllocator &allocator, ObDatum *datum,
                                     ObString &str)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (datum->is_null()) {
    str.reset();
  } else if (ob_is_text_tc(arg_expr.datum_meta_.type_) || arg_expr.obj_meta_.has_lob_header()) {
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
            ctx.exec_ctx_, allocator, *datum, arg_expr.datum_meta_,
            arg_expr.obj_meta_.has_lob_header(), str))) {
    }
  } else {
    str = datum->get_string();
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  INIT_SUCC(ret);
  ObDatumVector res_datum_vector = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObArray<ObString> group_model_ids;
  ObArray<int64_t> group_dims;
  ObArray<ObArray<ObString>> group_contents;
  ObArray<ObArray<int64_t>> group_row_idxs;
  if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 3
             && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObDatumVector model_vector = expr.locate_param_datumvector(ctx, MODEL_IDX);
    ObDatumVector content_vector = expr.locate_param_datumvector(ctx, CONTENT_IDX);
    ObDatumVector dim_vector;
    if (expr.arg_cnt_ == 3) {
      dim_vector = expr.locate_param_datumvector(ctx, DIM_IDX);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum *model_datum = model_vector.at(i);
      ObDatum *content_datum = content_vector.at(i);
      if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum) || content_datum->is_null()) {
        res_datum_vector.at(i)->set_null();
        eval_flags.set(i);
        continue;
      }
      ObString model_id;
      if (OB_FAIL(read_string_datum(*expr.args_[MODEL_IDX], ctx, temp_allocator,
                                    model_datum, model_id))) {
      } else if (model_id.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
      } else {
        ObString content;
        if (OB_FAIL(read_string_datum(*expr.args_[CONTENT_IDX], ctx, temp_allocator,
                                      content_datum, content))) {
        } else if (content.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
        } else {
          int64_t dim = 0;
          if (expr.arg_cnt_ == 3) {
            dim = dim_vector.at(i)->is_null() ? 0 : dim_vector.at(i)->get_int();
            if (dim <= 0) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
            }
          }
          if (OB_SUCC(ret)) {
            int64_t group_idx = -1;
            for (int64_t j = 0; group_idx < 0 && j < group_model_ids.count(); j++) {
              if (group_dims.at(j) == dim && group_model_ids.at(j).compare(model_id) == 0) {
                group_idx = j;
              }
            }
            if (group_idx < 0) {
              ObArray<ObString> contents;
              ObArray<int64_t> row_idxs;
              if (OB_FAIL(contents.push_back(content))) {
              } else if (OB_FAIL(row_idxs.push_back(i))) {
              } else if (OB_FAIL(group_contents.push_back(contents))) {
              } else if (OB_FAIL(group_row_idxs.push_back(row_idxs))) {
              } else if (OB_FAIL(group_model_ids.push_back(model_id))) {
              } else if (OB_FAIL(group_dims.push_back(dim))) {
              }
            } else if (OB_FAIL(group_contents.at(group_idx).push_back(content))) {
            } else if (OB_FAIL(group_row_idxs.at(group_idx).push_back(i))) {
            }
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else {
        eval_flags.set(i);
      }
    }
  }

  for (int64_t g = 0; OB_SUCC(ret) && g < group_model_ids.count(); g++) {
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = group_model_ids.at(g);
    if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObJsonObject *config = nullptr;
      int64_t dim = group_dims.at(g);
      if (dim > 0) {
        ObJsonInt *dim_json = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else {
        ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
        ObArray<ObString> results;
        if (OB_FAIL(model.call_dense_embedding_vector_v2(
                group_contents.at(g), config, results))) {
        } else if (results.count() != group_row_idxs.at(g).count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t k = 0; OB_SUCC(ret) && k < results.count(); k++) {
            int64_t row_idx = group_row_idxs.at(g).at(k);
            ObDatum *res_datum = res_datum_vector.at(row_idx);
            ObTextStringDatumResult text_result(expr.datum_meta_.type_, &expr, &ctx, res_datum);
            ObString result = results.at(k);
            if (OB_FAIL(text_result.init_with_batch_idx(result.length(), row_idx))) {
            } else if (OB_FAIL(text_result.append(result))) {
            } else {
              text_result.set_result();
              eval_flags.set(row_idx);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
