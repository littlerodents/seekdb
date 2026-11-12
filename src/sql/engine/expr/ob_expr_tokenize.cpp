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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

#include "lib/allocator/ob_malloc.h"
#include "lib/hash_func/murmur_hash.h"

namespace oceanbase
{
namespace sql
{
namespace
{
// ---- memo for tokenize() results ------------------------------------------
// tokenize() is a pure function of (text, parser, parser properties, collation).
// Workloads keep tokenizing the same documents (constant probes, index
// maintenance over unchanged rows), so a bounded memo turns the repeated work
// into one hash lookup: a hit skips parameter parsing, dictionary loading,
// segmentation and JSON packing.  Correctness relies on the key covering every
// input the slow path reads; nothing else influences the result.
struct TokenizeMemo
{
  static constexpr int64_t SLOT_COUNT = 1024;             // power of two
  static constexpr int64_t SLOT_MASK = SLOT_COUNT - 1;
  static constexpr int64_t MAX_VALUE_BYTES = 256 * 1024;  // huge documents stay uncached

  struct Slot
  {
    uint64_t hash1_;
    uint64_t hash2_;
    int64_t key_len_;
    char *key_;
    int64_t value_len_;
    char *value_;
    Slot() : hash1_(0), hash2_(0), key_len_(0), key_(nullptr), value_len_(0), value_(nullptr) {}
    ~Slot()
    {
      if (nullptr != key_) { ::ob_free(key_); }
      if (nullptr != value_) { ::ob_free(value_); }
    }
  };

  Slot slots_[SLOT_COUNT];
  pthread_mutex_t lock_;

  TokenizeMemo() { pthread_mutex_init(&lock_, nullptr); }
  ~TokenizeMemo() { pthread_mutex_destroy(&lock_); }

  static TokenizeMemo &instance()
  {
    static TokenizeMemo memo;
    return memo;
  }

  // Copies the cached bytes out under the lock and packs them after releasing it: the
  // pack writes into the expression frame and may pull in LOB handling, which must not
  // run while the memo lock is held across concurrent statements.
  bool try_hit(const common::ObString &key, const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
  {
    bool hit = false;
    const uint64_t h1 = common::murmurhash(key.ptr(), static_cast<int32_t>(key.length()), 0x20261111);
    const uint64_t h2 = common::murmurhash(key.ptr(), static_cast<int32_t>(key.length()), 0x9781a2b3);
    char stack_buf[2048];
    char *out = stack_buf;
    int64_t out_len = 0;
    pthread_mutex_lock(&lock_);
    Slot &slot = slots_[h1 & SLOT_MASK];
    if (h1 == slot.hash1_ && h2 == slot.hash2_
        && key.length() == slot.key_len_
        && 0 == MEMCMP(key.ptr(), slot.key_, key.length())
        && slot.value_len_ > 0) {
      if (slot.value_len_ <= static_cast<int64_t>(sizeof(stack_buf))) {
        MEMCPY(stack_buf, slot.value_, slot.value_len_);
        out_len = slot.value_len_;
        hit = true;
      } else {
        char *heap = static_cast<char *>(ob_malloc(slot.value_len_, common::ObMemAttr("TokMemoOut")));
        if (nullptr != heap) {
          MEMCPY(heap, slot.value_, slot.value_len_);
          out = heap;
          out_len = slot.value_len_;
          hit = true;
        }
      }
    }
    pthread_mutex_unlock(&lock_);
    if (hit) {
      const common::ObString value(out_len, out);
      if (OB_SUCCESS != ObJsonExprHelper::pack_json_str_res(expr, ctx, res, value)) {
        hit = false;
      }
      if (out != stack_buf) {
        ob_free(out);
      }
    }
    return hit;
  }

  void insert(const common::ObString &key, const common::ObString &value)
  {
    if (value.length() <= 0 || value.length() > MAX_VALUE_BYTES) {
      return;
    }
    const uint64_t h1 = common::murmurhash(key.ptr(), static_cast<int32_t>(key.length()), 0x20261111);
    const uint64_t h2 = common::murmurhash(key.ptr(), static_cast<int32_t>(key.length()), 0x9781a2b3);
    char *key_buf = static_cast<char *>(ob_malloc(key.length(), common::ObMemAttr("TokMemo")));
    char *val_buf = static_cast<char *>(ob_malloc(value.length(), common::ObMemAttr("TokMemo")));
    if (nullptr == key_buf || nullptr == val_buf) {
      if (nullptr != key_buf) { ob_free(key_buf); }
      if (nullptr != val_buf) { ob_free(val_buf); }
      return;
    }
    MEMCPY(key_buf, key.ptr(), key.length());
    MEMCPY(val_buf, value.ptr(), value.length());
    pthread_mutex_lock(&lock_);
    Slot &slot = slots_[h1 & SLOT_MASK];
    if (nullptr != slot.key_) { ob_free(slot.key_); }
    if (nullptr != slot.value_) { ob_free(slot.value_); }
    slot.hash1_ = h1;
    slot.hash2_ = h2;
    slot.key_len_ = key.length();
    slot.key_ = key_buf;
    slot.value_len_ = value.length();
    slot.value_ = val_buf;
    pthread_mutex_unlock(&lock_);
  }
};

// Serializes every input the tokenize path depends on into a fixed layout binary
// key: raw field writes only, no formatting, so the hit path stays a few memcpys.
// Argument evaluation errors are not reported here: the slow path re-evaluates the
// same arguments and surfaces the error exactly as before.
bool build_tokenize_memo_key(const ObExpr &expr, ObEvalCtx &ctx, common::ObSqlString &key)
{
  bool usable = false;
  int ret = OB_SUCCESS;
  ObDatum *text = nullptr;
  ObDatum *parser = nullptr;
  ObDatum *props = nullptr;
  uint8_t flags = 0;
  do {
    if (OB_FAIL(expr.args_[0]->eval(ctx, text))) {
      break;
    } else if (nullptr != text && text->is_null()) {
      flags |= 0x1;
    }
    if (expr.arg_cnt_ >= 2) {
      flags |= 0x2;
      if (OB_FAIL(expr.args_[1]->eval(ctx, parser))) {
        break;
      } else if (nullptr != parser && parser->is_null()) {
        flags |= 0x4;
      }
    }
    if (expr.arg_cnt_ >= 3) {
      flags |= 0x8;
      if (OB_FAIL(expr.args_[2]->eval(ctx, props))) {
        break;
      } else if (nullptr != props && props->is_null()) {
        flags |= 0x10;
      }
    }
    const uint32_t collation = static_cast<uint32_t>(expr.args_[0]->obj_meta_.get_collation_type());
    const common::ObString empty;
    const common::ObString &text_str = (flags & 0x1) ? empty : text->get_string();
    const common::ObString &parser_str = (expr.arg_cnt_ < 2 || (flags & 0x4)) ? empty
                                                                             : parser->get_string();
    const common::ObString &props_str = (expr.arg_cnt_ < 3 || (flags & 0x10)) ? empty
                                                                             : props->get_string();
    const int32_t lens[3] = {static_cast<int32_t>(text_str.length()),
                             static_cast<int32_t>(parser_str.length()),
                             static_cast<int32_t>(props_str.length())};
    const common::ObString *parts[3] = {&text_str, &parser_str, &props_str};
    if (OB_FAIL(key.reserve(16 + lens[0] + lens[1] + lens[2]))) {
      break;
    } else if (OB_FAIL(key.append(reinterpret_cast<const char *>(&collation), sizeof(collation)))
               || OB_FAIL(key.append(reinterpret_cast<const char *>(&flags), sizeof(flags)))) {
      break;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
      if (OB_FAIL(key.append(reinterpret_cast<const char *>(&lens[i]), sizeof(lens[i])))
          || OB_FAIL(key.append(*parts[i]))) {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      usable = true;
    }
  } while (false);
  return usable;
}

} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  common::ObSqlString memo_key;
  const bool memo_usable = (expr.arg_cnt_ >= 1 && expr.arg_cnt_ <= 3)
      && build_tokenize_memo_key(expr, ctx, memo_key);
  if (memo_usable && TokenizeMemo::instance().try_hit(memo_key.string(), expr, ctx, expr_datum)) {
    return ret;
  }

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param(temp_allocator);

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_ISNULL(json_result)) {
    expr_datum.set_null();
  } else {
    common::ObString bin;
    if (OB_FAIL(json_result->get_raw_binary(bin, &temp_allocator))) {
      LOG_WARN("fail to serialize tokenize result", K(ret));
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, bin))) {
      LOG_WARN("fail to pack tokenize result", K(ret));
    } else if (memo_usable) {
      TokenizeMemo::instance().insert(memo_key.string(), bin);
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_, param.parsed_properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam::TokenizeParam(ObIAllocator &allocator)
  : allocator_(allocator),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_.parse_from_str(parser_name_.ptr(), parser_name_.length()))) {
  } else if (properties.empty()) {
    // No options to validate: use the same defaults without building JSON.
    ret = parsed_properties_.parse_for_parser_helper(parser_, parser_properties, true);
  } else if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parsed_properties_.parse_for_parser_helper(parser_, parser_properties))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
