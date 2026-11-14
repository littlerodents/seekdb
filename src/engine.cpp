// Statement execution for MiniVectorDB.
#include "engine.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstring>
#include <map>

namespace mvdb {
namespace {

Result make_error(const char *code, const char *message) {
  Result r;
  r.ok = false;
  r.error_code = code;
  r.error_message = message;
  return r;
}

// ---------- redo payload encoding ----------
// The payload is a compact tag plus the statement's effect, so recovery can rebuild the
// catalogue without a SQL parser round-trip.
enum RedoOp : uint8_t {
  REDO_CREATE_TABLE = 1,
  REDO_INSERT_ROWS = 2,
  REDO_UPDATE_ROWS = 3,
  REDO_DELETE_ROWS = 4,
  REDO_CREATE_INDEX = 5,
};

void put_u32(std::string &out, uint32_t v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}
void put_u64(std::string &out, uint64_t v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}
void put_str(std::string &out, const std::string &s) {
  put_u32(out, static_cast<uint32_t>(s.size()));
  out.append(s.data(), s.size());
}
bool get_u32(const char *&p, const char *end, uint32_t &v) {
  if (end - p < static_cast<ptrdiff_t>(sizeof(v))) {
    return false;
  }
  std::memcpy(&v, p, sizeof(v));
  p += sizeof(v);
  return true;
}
bool get_u64(const char *&p, const char *end, uint64_t &v) {
  if (end - p < static_cast<ptrdiff_t>(sizeof(v))) {
    return false;
  }
  std::memcpy(&v, p, sizeof(v));
  p += sizeof(v);
  return true;
}
bool get_str(const char *&p, const char *end, std::string &s) {
  uint32_t len = 0;
  if (!get_u32(p, end, len) || end - p < static_cast<ptrdiff_t>(len)) {
    return false;
  }
  s.assign(p, len);
  p += len;
  return true;
}

// ---------- helpers ----------

bool value_matches(const Value &row_value, CmpOp op, const Value &lit, bool &match,
                   std::string &err) {
  match = false;
  if (lit.is_null) {
    // Comparisons against NULL are not part of the subset; treat as no match.
    return true;
  }
  if (row_value.type != lit.type) {
    // A string literal compared to a BIGINT column is coerced only when it parses.
    if (row_value.type == ColType::BIGINT && lit.type == ColType::VARCHAR) {
      errno = 0;
      char *endp = nullptr;
      const long long v = std::strtoll(lit.s.c_str(), &endp, 10);
      if (errno != 0 || endp == lit.s.c_str() || *endp != '\0') {
        err = "type mismatch";
        return false;
      }
      const int64_t rv = row_value.i;
      const int64_t lv = static_cast<int64_t>(v);
      switch (op) {
        case CmpOp::EQ: match = rv == lv; break;
        case CmpOp::LT: match = rv < lv; break;
        case CmpOp::LE: match = rv <= lv; break;
        case CmpOp::GT: match = rv > lv; break;
        case CmpOp::GE: match = rv >= lv; break;
      }
      return true;
    }
    err = "type mismatch";
    return false;
  }
  switch (row_value.type) {
    case ColType::BIGINT: {
      const int64_t rv = row_value.i;
      const int64_t lv = lit.i;
      switch (op) {
        case CmpOp::EQ: match = rv == lv; break;
        case CmpOp::LT: match = rv < lv; break;
        case CmpOp::LE: match = rv <= lv; break;
        case CmpOp::GT: match = rv > lv; break;
        case CmpOp::GE: match = rv >= lv; break;
      }
      break;
    }
    case ColType::VARCHAR: {
      const int cmp = compare_varchar(row_value.s, lit.s);
      switch (op) {
        case CmpOp::EQ: match = cmp == 0; break;
        case CmpOp::LT: match = cmp < 0; break;
        case CmpOp::LE: match = cmp <= 0; break;
        case CmpOp::GT: match = cmp > 0; break;
        case CmpOp::GE: match = cmp >= 0; break;
      }
      break;
    }
    case ColType::DOUBLE: {
      const double rv = row_value.d;
      const double lv = lit.type == ColType::DOUBLE ? lit.d : static_cast<double>(lit.i);
      switch (op) {
        case CmpOp::EQ: match = rv == lv; break;
        case CmpOp::LT: match = rv < lv; break;
        case CmpOp::LE: match = rv <= lv; break;
        case CmpOp::GT: match = rv > lv; break;
        case CmpOp::GE: match = rv >= lv; break;
      }
      break;
    }
    case ColType::VECTOR:
      err = "vector comparison is not supported";
      return false;
  }
  return true;
}

bool row_matches(const Table &table, const Row &row, const WhereClause &where,
                 std::string &err) {
  if (!where.present) {
    return true;
  }
  for (const auto &c : where.conds) {
    const int64_t pos = table.schema.column_pos(c.column);
    if (pos < 0) {
      err = "unknown column " + c.column;
      return false;
    }
    bool match = false;
    if (!value_matches(row[static_cast<size_t>(pos)], c.op, c.literal, match, err)) {
      return false;
    }
    if (!match) {
      return false;
    }
  }
  return true;
}

// Coerces a literal into a column's declared type, rejecting values the column cannot hold.
bool coerce_for_column(const Stmt::ColumnDef &col, const Value &in, Value &out,
                       std::string &err) {
  out = in;
  switch (col.type) {
    case ColType::BIGINT:
      if (in.type != ColType::BIGINT) {
        err = "expected BIGINT value";
        return false;
      }
      return true;
    case ColType::VARCHAR:
      if (in.type != ColType::VARCHAR) {
        err = "expected VARCHAR value";
        return false;
      }
      if (col.length > 0 && utf8_length(in.s) > col.length) {
        err = "value too long for column";
        return false;
      }
      return true;
    case ColType::DOUBLE:
      err = "DOUBLE is not a storable column type";
      return false;
    case ColType::VECTOR: {
      if (in.type == ColType::VECTOR) {
        if (col.length > 0 && static_cast<int64_t>(in.v.size()) != col.length) {
          err = "vector dimension mismatch";
          return false;
        }
        return true;
      }
      if (in.type == ColType::VARCHAR) {
        std::vector<float> vec;
        std::string verr;
        if (!parse_vector_literal(in.s, vec, verr)) {
          err = verr;
          return false;
        }
        if (col.length > 0 && static_cast<int64_t>(vec.size()) != col.length) {
          err = "vector dimension mismatch";
          return false;
        }
        out = Value::make_vector(std::move(vec));
        return true;
      }
      err = "expected VECTOR value";
      return false;
    }
  }
  err = "unsupported column type";
  return false;
}

}  // namespace

bool Storage::apply_redo_payload(const char *payload, size_t len) {
  const char *p = payload;
  const char *end = payload + len;
  if (end - p < 1) {
    return false;
  }
  const uint8_t op = static_cast<uint8_t>(*p++);
  switch (op) {
    case REDO_CREATE_TABLE: {
      Table table;
      if (!decode_schema(p, end, table.schema)) {
        return false;
      }
      const std::string name = table.schema.name;
      tables_[name] = std::move(table);
      return true;
    }
    case REDO_CREATE_INDEX: {
      std::string table_name;
      std::string index_name;
      if (!get_str(p, end, table_name) || !get_str(p, end, index_name)) {
        return false;
      }
      Table *tbl = find_table(table_name);
      if (tbl == nullptr) {
        return false;
      }
      tbl->schema.index_names.push_back(index_name);
      return true;
    }
    case REDO_INSERT_ROWS: {
      std::string table_name;
      uint32_t nrow = 0;
      if (!get_str(p, end, table_name) || !get_u32(p, end, nrow)) {
        return false;
      }
      Table *tbl = find_table(table_name);
      if (tbl == nullptr) {
        return false;
      }
      for (uint32_t r = 0; r < nrow; ++r) {
        Row row;
        if (!decode_row(p, end, row, tbl->schema.columns.size())) {
          return false;
        }
        const int64_t pk = row[tbl->schema.pk_index].i;
        std::string err;
        if (!put_row(table_name, pk, row, err)) {
          return false;
        }
      }
      return true;
    }
    case REDO_UPDATE_ROWS: {
      std::string table_name;
      uint32_t nrow = 0;
      if (!get_str(p, end, table_name) || !get_u32(p, end, nrow)) {
        return false;
      }
      Table *tbl = find_table(table_name);
      if (tbl == nullptr) {
        return false;
      }
      for (uint32_t r = 0; r < nrow; ++r) {
        Row row;
        if (!decode_row(p, end, row, tbl->schema.columns.size())) {
          return false;
        }
        const int64_t pk = row[tbl->schema.pk_index].i;
        std::string err;
        if (!put_row(table_name, pk, row, err)) {
          return false;
        }
      }
      return true;
    }
    case REDO_DELETE_ROWS: {
      std::string table_name;
      uint32_t nrow = 0;
      if (!get_str(p, end, table_name) || !get_u32(p, end, nrow)) {
        return false;
      }
      Table *tbl = find_table(table_name);
      if (tbl == nullptr) {
        return false;
      }
      for (uint32_t r = 0; r < nrow; ++r) {
        uint64_t pk = 0;
        if (!get_u64(p, end, pk)) {
          return false;
        }
        std::string err;
        if (!erase_row(table_name, static_cast<int64_t>(pk), err)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

bool Engine::open(const std::string &dir, std::string &err) { return storage_.open(dir, err); }

bool Engine::shutdown(std::string &err) {
  // A shutdown rolls back an open transaction first: its writes were never acknowledged,
  // so they must not reach the checkpoint either.
  if (in_tx_) {
    exec_rollback();
  }
  return storage_.checkpoint(err);
}

void Engine::encode_redo(const Stmt &stmt, std::string &out) {
  out.clear();
  switch (stmt.kind) {
    case Stmt::Kind::CREATE_TABLE: {
      out.push_back(static_cast<char>(REDO_CREATE_TABLE));
      const Table *t = storage_.find_table(stmt.table);
      if (t != nullptr) {
        encode_schema(out, t->schema);
      }
      break;
    }
    case Stmt::Kind::CREATE_VECTOR_INDEX: {
      out.push_back(static_cast<char>(REDO_CREATE_INDEX));
      put_str(out, stmt.table);
      put_str(out, stmt.index_name);
      break;
    }
    case Stmt::Kind::INSERT: {
      out.push_back(static_cast<char>(REDO_INSERT_ROWS));
      put_str(out, stmt.table);
      const Table *t = storage_.find_table(stmt.table);
      put_u32(out, static_cast<uint32_t>(stmt.rows.size()));
      if (t != nullptr) {
        for (const auto &raw : stmt.rows) {
          Row row;
          for (size_t k = 0; k < raw.size() && k < t->schema.columns.size(); ++k) {
            Value v;
            std::string ignored;
            coerce_for_column(t->schema.columns[k], raw[k], v, ignored);
            row.push_back(std::move(v));
          }
          encode_row(out, row);
        }
      }
      break;
    }
    default:
      break;
  }
}

// Durability gate for one statement's effect: outside a transaction the payload is
// written through immediately; inside one it is buffered until COMMIT.
bool Engine::persist_or_buffer(const std::string &payload, std::string &err) {
  bool ok = true;
  if (in_tx_) {
    tx_redo_.push_back(payload);
  } else {
    ok = storage_.append_redo(payload, err);
  }
  return ok;
}

Result Engine::exec_create_table(const Stmt &stmt) {
  if (storage_.has_table(stmt.table)) {
    return make_error("table_exists", "table already exists");
  }
  Table table;
  table.schema.name = stmt.table;
  table.schema.columns = stmt.columns;
  int64_t pk_count = 0;
  int64_t vector_count = 0;
  for (size_t k = 0; k < table.schema.columns.size(); ++k) {
    if (table.schema.columns[k].primary_key) {
      table.schema.pk_index = static_cast<int64_t>(k);
      ++pk_count;
      if (table.schema.columns[k].type != ColType::BIGINT) {
        return make_error("invalid_schema", "primary key must be BIGINT");
      }
    }
    if (table.schema.columns[k].type == ColType::VECTOR) {
      ++vector_count;
      if (table.schema.vector_index < 0) {
        table.schema.vector_index = static_cast<int64_t>(k);
      }
    }
  }
  if (pk_count != 1) {
    return make_error("invalid_schema", "exactly one primary key is required");
  }
  if (vector_count > 1) {
    return make_error("invalid_schema", "at most one vector column is allowed");
  }
  storage_.create_table(table.schema);
  Result r;
  return r;
}

Result Engine::exec_create_index(const Stmt &stmt) {
  Table *t = storage_.find_table(stmt.table);
  if (t == nullptr) {
    return make_error("no_such_table", "table does not exist");
  }
  const int64_t pos = t->schema.column_pos(stmt.index_column);
  if (pos < 0) {
    return make_error("no_such_column", "column does not exist");
  }
  if (t->schema.columns[static_cast<size_t>(pos)].type != ColType::VECTOR) {
    return make_error("invalid_index", "vector index requires a vector column");
  }
  t->schema.index_names.push_back(stmt.index_name);
  Result r;
  return r;
}

// The task's INSERT may name columns in any order.  Everything downstream (coercion, redo
// payload, duplicate-key detection) assumes table-definition order, so the rows are permuted
// once here instead of teaching each consumer about the column list.
bool Engine::normalize_insert_columns(Stmt &stmt, std::string &err) {
  const Table *t = storage_.find_table(stmt.table);
  if (t == nullptr) {
    return true;  // exec_insert reports no_such_table
  }
  const size_t ncol = t->schema.columns.size();
  if (stmt.insert_columns.size() != ncol) {
    err = "all columns must be provided";
    return false;
  }
  std::vector<size_t> source(ncol, ncol);  // table column -> position in the value list
  for (size_t k = 0; k < stmt.insert_columns.size(); ++k) {
    const std::string &name = stmt.insert_columns[k];
    size_t target = ncol;
    for (size_t j = 0; j < ncol; ++j) {
      if (TableSchema::equal_ci(t->schema.columns[j].name, name)) {
        target = j;
        break;
      }
    }
    if (target == ncol) {
      err = "unknown column: " + name;
      return false;
    }
    if (source[target] != ncol) {
      err = "duplicate column: " + name;
      return false;
    }
    source[target] = k;
  }
  for (auto &row : stmt.rows) {
    if (row.size() != ncol) {
      err = "value count does not match the column list";
      return false;
    }
    std::vector<Value> ordered(ncol);
    for (size_t j = 0; j < ncol; ++j) {
      ordered[j] = std::move(row[source[j]]);
    }
    row = std::move(ordered);
  }
  stmt.insert_columns.clear();
  return true;
}

Result Engine::exec_insert(const Stmt &stmt) {
  Table *t = storage_.find_table(stmt.table);
  if (t == nullptr) {
    return make_error("no_such_table", "table does not exist");
  }
  const size_t ncol = t->schema.columns.size();
  // Validate every row before applying any of them: a multi-row INSERT is one statement
  // and must not leave a partial effect behind.
  std::vector<Row> prepared;
  prepared.reserve(stmt.rows.size());
  std::map<int64_t, bool> seen;
  for (const auto &raw : stmt.rows) {
    if (raw.size() != ncol) {
      return make_error("column_count_mismatch", "value count does not match the table");
    }
    Row row;
    row.reserve(ncol);
    for (size_t k = 0; k < ncol; ++k) {
      Value v;
      std::string err;
      if (!coerce_for_column(t->schema.columns[k], raw[k], v, err)) {
        return make_error("invalid_value", err.c_str());
      }
      row.push_back(std::move(v));
    }
    const int64_t pk = row[static_cast<size_t>(t->schema.pk_index)].i;
    if (storage_.has_row(stmt.table, pk) || seen.find(pk) != seen.end()) {
      return make_error("duplicate_key", "duplicate primary key");
    }
    seen[pk] = true;
    prepared.push_back(std::move(row));
  }
  for (auto &row : prepared) {
    const int64_t pk = row[static_cast<size_t>(t->schema.pk_index)].i;
    // Validated above, so the row is new: the undo entry records that it must disappear again.
    note_undo(stmt.table, pk);
    std::string err;
    if (!storage_.put_row(stmt.table, pk, row, err)) {
      return make_error("io_error", err.c_str());
    }
  }
  Result r;
  r.has_affected_rows = true;
  r.affected_rows = static_cast<int64_t>(prepared.size());
  return r;
}

Result Engine::exec_update(const Stmt &stmt, std::vector<Row> &changed) {
  Table *t = storage_.find_table(stmt.table);
  if (t == nullptr) {
    return make_error("no_such_table", "table does not exist");
  }
  // Resolve assignment columns first.
  std::vector<std::pair<size_t, Value>> updates;
  for (const auto &a : stmt.assignments) {
    const int64_t pos = t->schema.column_pos(a.column);
    if (pos < 0) {
      return make_error("no_such_column", "column does not exist");
    }
    if (static_cast<int64_t>(pos) == t->schema.pk_index) {
      return make_error("unsupported_sql", "primary key update is not supported");
    }
    Value v;
    std::string err;
    if (!coerce_for_column(t->schema.columns[static_cast<size_t>(pos)], a.literal, v, err)) {
      return make_error("invalid_value", err.c_str());
    }
    updates.emplace_back(static_cast<size_t>(pos), std::move(v));
  }
  // Collect every matching row before touching any of them: a WHERE that fails part-way through
  // must leave the statement with no effect at all, which statement-level atomicity requires.
  // Rows are materialised one at a time and dropped again, so the scan does not hold the table.
  std::vector<int64_t> hits;
  for (const auto &kv : t->rows) {
    Row row;
    if (!storage_.get_row_at(t->schema, kv.second, row)) {
      return make_error("io_error", "cannot read row");
    }
    std::string err;
    if (!row_matches(*t, row, stmt.where, err)) {
      if (!err.empty()) {
        return make_error("invalid_where", err.c_str());
      }
      continue;
    }
    hits.push_back(kv.first);
  }
  for (int64_t pk : hits) {
    Row row;
    if (!storage_.get_row(stmt.table, pk, row)) {
      continue;
    }
    note_undo(stmt.table, pk);
    for (const auto &u : updates) {
      row[u.first] = u.second;
    }
    std::string werr;
    if (!storage_.put_row(stmt.table, pk, row, werr)) {
      return make_error("io_error", werr.c_str());
    }
    // Report the changed rows directly.  The redo encoder used to take a deep copy of the whole
    // table and diff against it, which at 1.2M rows allocates hundreds of megabytes per
    // statement and was what the kernel killed.
    changed.push_back(std::move(row));
  }
  Result r;
  r.has_affected_rows = true;
  r.affected_rows = static_cast<int64_t>(hits.size());
  return r;
}

Result Engine::exec_delete(const Stmt &stmt, std::vector<int64_t> &removed) {
  Table *t = storage_.find_table(stmt.table);
  if (t == nullptr) {
    return make_error("no_such_table", "table does not exist");
  }
  std::vector<int64_t> doomed;
  for (const auto &kv : t->rows) {
    Row row;
    if (!storage_.get_row_at(t->schema, kv.second, row)) {
      return make_error("io_error", "cannot read row");
    }
    std::string err;
    if (!row_matches(*t, row, stmt.where, err)) {
      if (!err.empty()) {
        return make_error("invalid_where", err.c_str());
      }
      continue;
    }
    doomed.push_back(kv.first);
  }
  for (int64_t pk : doomed) {
    note_undo(stmt.table, pk);
    std::string derr;
    if (!storage_.erase_row(stmt.table, pk, derr)) {
      return make_error("io_error", derr.c_str());
    }
  }
  // The deleted keys are reported straight out, instead of being recovered by diffing a
  // full-table copy the way the redo encoder used to.
  removed = doomed;
  Result r;
  r.has_affected_rows = true;
  r.affected_rows = static_cast<int64_t>(doomed.size());
  return r;
}

Result Engine::exec_select(const Stmt &stmt, const std::string &search_mode) {
  (void)search_mode;  // the exact plan answers both modes, which the task allows
  const Table *t = storage_.find_table(stmt.table);
  if (t == nullptr) {
    return make_error("no_such_table", "table does not exist");
  }
  // Expand SELECT * into the declared column list.
  std::vector<SelectItem> items = stmt.select_items;
  if (items.size() == 1 && items[0].star) {
    items.clear();
    for (const auto &c : t->schema.columns) {
      SelectItem it;
      it.column = c.name;
      items.push_back(std::move(it));
    }
  }
  // Validate referenced columns.
  for (const auto &it : items) {
    if (it.is_count) {
      if (!it.count_star) {
        const int64_t pos = t->schema.column_pos(it.count_column);
        if (pos < 0) {
          return make_error("no_such_column", "column does not exist");
        }
      }
      continue;
    }
    const std::string &name = it.is_l2 ? it.l2_column : it.column;
    const int64_t pos = t->schema.column_pos(name);
    if (pos < 0) {
      return make_error("no_such_column", "column does not exist");
    }
    if (it.is_l2 && t->schema.columns[static_cast<size_t>(pos)].type != ColType::VECTOR) {
      return make_error("invalid_argument", "L2_DISTANCE requires a vector column");
    }
  }
  // Filter.  Only primary keys are collected here; each row is materialised when it is actually
  // needed.  Keeping whole rows for every match would put the table back in memory, which is the
  // thing the disk-resident layout exists to avoid.
  std::vector<const int64_t *> matched;
  std::vector<const Row *> matched_rows;  // parallel to `matched` when the scan cache is used
  matched.reserve(t->rows.size());
  // Reuse the decoded table when possible; otherwise decode once and keep it for the queries
  // that follow (bounded so a table larger than the memory budget keeps streaming).
  constexpr size_t kScanCacheBudgetBytes = 64u << 20;
  bool use_cache = scan_cache_.epoch == mutation_epoch_ && scan_cache_.table == stmt.table;
  if (!use_cache) {
    scan_cache_.rows.clear();
    scan_cache_.table.clear();
    size_t bytes = 0;
    for (const auto &c : t->schema.columns) {
      bytes += (c.type == ColType::VECTOR) ? sizeof(float) * static_cast<size_t>(c.length)
                                           : (c.type == ColType::VARCHAR ? c.length + 16 : 16);
    }
    bytes += 48;  // per-row bookkeeping
    if (bytes * t->rows.size() <= kScanCacheBudgetBytes) {
      scan_cache_.rows.reserve(t->rows.size());
      bool ok = true;
      for (const auto &kv : t->rows) {
        Row row;
        if (!storage_.get_row_at(t->schema, kv.second, row)) {
          ok = false;
          break;
        }
        scan_cache_.rows.emplace_back(kv.first, std::move(row));
      }
      if (ok) {
        scan_cache_.table = stmt.table;
        scan_cache_.epoch = mutation_epoch_;
        use_cache = true;
      } else {
        scan_cache_.rows.clear();
      }
    }
  }
  if (use_cache) {
    for (const auto &entry : scan_cache_.rows) {
      std::string err;
      if (!row_matches(*t, entry.second, stmt.where, err)) {
        if (!err.empty()) {
          return make_error("invalid_where", err.c_str());
        }
        continue;
      }
      matched.push_back(&entry.first);
      if (use_cache) {
        matched_rows.push_back(&entry.second);
      }
    }
  } else {
    for (const auto &kv : t->rows) {
      Row row;
      if (!storage_.get_row_at(t->schema, kv.second, row)) {
        return make_error("io_error", "cannot read row");
      }
      std::string err;
      if (!row_matches(*t, row, stmt.where, err)) {
        if (!err.empty()) {
          return make_error("invalid_where", err.c_str());
        }
        continue;
      }
      matched.push_back(&kv.first);
    }
  }
  // An aggregate collapses the match set into one row, so it bypasses ordering and LIMIT: there
  // is exactly one result row and it always carries its column name.
  bool has_count = false;
  for (const auto &it : items) {
    if (it.is_count) {
      has_count = true;
      break;
    }
  }
  if (has_count) {
    Result r;
    std::vector<Value> out;
    for (const auto &it : items) {
      std::string name = it.alias;
      if (name.empty()) {
        name = it.is_count ? "count(*)" : "count";
      }
      r.columns.push_back(name);
      if (it.is_count) {
        out.push_back(Value::make_bigint(static_cast<int64_t>(matched.size())));
      }
    }
    r.rows.push_back(std::move(out));
    return r;
  }
  // ORDER BY resolves to either a projected column/alias or an L2_DISTANCE term.  The term
  // may be written inline (ORDER BY L2_DISTANCE(col,'[...]')) or refer to a projected
  // L2_DISTANCE alias; both are kept as keys that already carry their own query vector.
  struct SortKey {
    bool by_l2 = false;
    size_t item_pos = 0;
    bool desc = false;
    const std::vector<float> *l2_query = nullptr;  // set when the term is inline
    int64_t l2_col_pos = -1;
  };
  std::vector<SortKey> keys;
  for (const auto &o : stmt.order_by) {
    SortKey key;
    key.desc = o.desc;
    if (o.is_l2) {
      key.by_l2 = true;
      key.l2_query = &o.l2_query;
      key.l2_col_pos = t->schema.column_pos(o.l2_column);
      if (key.l2_col_pos < 0) {
        return make_error("no_such_column", "order by L2_DISTANCE column does not exist");
      }
      keys.push_back(key);
      continue;
    }
    bool found = false;
    for (size_t k = 0; k < items.size(); ++k) {
      const std::string &alias = items[k].alias;
      const std::string &col = items[k].is_l2 ? items[k].l2_column : items[k].column;
      if (TableSchema::equal_ci(o.column, alias) || TableSchema::equal_ci(o.column, col) ||
          (items[k].is_l2 && TableSchema::equal_ci(o.column, "distance"))) {
        key.by_l2 = items[k].is_l2;
        key.item_pos = k;
        found = true;
        break;
      }
    }
    if (!found) {
      // Allow ordering by a table column that is not projected.
      const int64_t pos = t->schema.column_pos(o.column);
      if (pos < 0) {
        return make_error("no_such_column", "order by column does not exist");
      }
      key.by_l2 = false;
      key.item_pos = items.size() + static_cast<size_t>(pos);
    }
    keys.push_back(key);
  }
  auto value_of = [&](const Row &row, const SortKey &key) -> const Value * {
    if (key.by_l2) {
      return nullptr;
    }
    if (key.item_pos < items.size()) {
      const SelectItem &it = items[key.item_pos];
      const int64_t pos = t->schema.column_pos(it.is_l2 ? it.l2_column : it.column);
      return pos < 0 ? nullptr : &row[static_cast<size_t>(pos)];
    }
    const size_t pos = key.item_pos - items.size();
    return pos < row.size() ? &row[pos] : nullptr;
  };
  auto dist_sq_of = [&](const Row &row, const SortKey &key) {
    if (key.l2_query != nullptr) {
      return l2_distance_sq(row[static_cast<size_t>(key.l2_col_pos)].v, *key.l2_query);
    }
    const SelectItem &it = items[key.item_pos];
    const int64_t pos = t->schema.column_pos(it.l2_column);
    return l2_distance_sq(row[static_cast<size_t>(pos)].v, it.l2_query);
  };
  // Sorting keys are computed once per row instead of re-reading rows inside the comparator.  A
  // comparator that reads from disk is not usable with std::sort: a failed read would have to
  // fall back to some other order, which breaks the strict weak ordering that sort requires.
  // Fast path for the benchmark shape: one L2 key, ascending, bounded LIMIT.  A k-element heap
  // over the (already cached) rows avoids materialising one SortValue vector per candidate and
  // sorting the whole match set for a ten-row answer.
  if (keys.size() == 1 && keys[0].by_l2 && !keys[0].desc && stmt.limit > 0) {
    struct Cand {
      double d;
      const int64_t *pk;
    };
    const auto cmp = [](const Cand &a, const Cand &b) {
      if (a.d != b.d) {
        return a.d < b.d;
      }
      return *a.pk < *b.pk;
    };
    const size_t k = static_cast<size_t>(stmt.limit);
    std::vector<Cand> heap;
    heap.reserve(k + 1);
    bool fast_error = false;
    for (size_t mi = 0; mi < matched.size(); ++mi) {
      Row row_storage;
      const Row *rowp = nullptr;
      if (use_cache && mi < matched_rows.size()) {
        rowp = matched_rows[mi];
      } else {
        if (!storage_.get_row(stmt.table, *matched[mi], row_storage)) {
          fast_error = true;
          break;
        }
        rowp = &row_storage;
      }
      const Cand c{dist_sq_of(*rowp, keys[0]), matched[mi]};
      if (heap.size() < k) {
        heap.push_back(c);
        std::push_heap(heap.begin(), heap.end(), cmp);
      } else if (cmp(c, heap.front())) {
        std::pop_heap(heap.begin(), heap.end(), cmp);
        heap.back() = c;
        std::push_heap(heap.begin(), heap.end(), cmp);
      }
    }
    if (!fast_error) {
      std::sort_heap(heap.begin(), heap.end(), cmp);
      matched.clear();
      matched.reserve(heap.size());
      for (const auto &c : heap) {
        matched.push_back(c.pk);
      }
      Result fr;
      for (const auto &it : items) {
        std::string name = it.alias;
        if (name.empty()) {
          name = it.is_l2 ? "distance" : it.column;
        }
        fr.columns.push_back(name);
      }
      for (const auto *pk : matched) {
        Row row;
        if (storage_.get_row(stmt.table, *pk, row)) {
        } else {
          return make_error("io_error", "cannot read row");
        }
        std::vector<Value> out;
        out.reserve(items.size());
        for (const auto &it : items) {
          if (it.is_l2) {
            const int64_t pos = t->schema.column_pos(it.l2_column);
            out.push_back(Value::make_double(l2_distance(row[static_cast<size_t>(pos)].v, it.l2_query)));
          } else {
            const int64_t pos = t->schema.column_pos(it.column);
            out.push_back(row[static_cast<size_t>(pos)]);
          }
        }
        fr.rows.push_back(std::move(out));
      }
      return fr;
    }
  }
  struct SortValue {
    bool numeric = true;   // BIGINT comparison, otherwise text
    bool is_l2 = false;
    double dist = 0.0;
    int64_t num = 0;
    std::string text;
  };
  struct Ranked {
    const int64_t *pk;
    std::vector<SortValue> keys;
  };
  std::vector<Ranked> ranked;
  ranked.reserve(matched.size());
  bool key_error = false;
  for (size_t mi = 0; mi < matched.size(); ++mi) {
    const int64_t *pk = matched[mi];
    Row row_storage;
    const Row *rowp = nullptr;
    if (use_cache && mi < matched_rows.size()) {
      rowp = matched_rows[mi];
    } else {
      if (!storage_.get_row(stmt.table, *pk, row_storage)) {
        key_error = true;
        break;
      }
      rowp = &row_storage;
    }
    const Row &row = *rowp;
    Ranked r;
    r.pk = pk;
    r.keys.reserve(keys.size());
    for (const auto &key : keys) {
      SortValue sv;
      if (key.by_l2) {
        sv.is_l2 = true;
        sv.dist = dist_sq_of(row, key);
      } else if (const Value *v = value_of(row, key)) {
        if (v->type == ColType::VARCHAR) {
          sv.numeric = false;
          sv.text = v->s;
        } else if (v->type == ColType::BIGINT) {
          sv.numeric = true;
          sv.num = v->i;
        } else {
          sv.numeric = true;
          sv.num = 0;
        }
      }
      r.keys.push_back(std::move(sv));
    }
    ranked.push_back(std::move(r));
  }
  if (key_error) {
    return make_error("io_error", "cannot read row");
  }
  const auto cmp_ranked = [&](const Ranked &a, const Ranked &b) {
    for (size_t k = 0; k < keys.size(); ++k) {
      const SortValue &va = a.keys[k];
      const SortValue &vb = b.keys[k];
      if (va.is_l2) {
        const double eps = 1e-12;
        if (va.dist < vb.dist - eps) {
          return !keys[k].desc;
        }
        if (va.dist > vb.dist + eps) {
          return keys[k].desc;
        }
        continue;
      }
      int c = 0;
      if (va.numeric && vb.numeric) {
        c = va.num < vb.num ? -1 : (va.num > vb.num ? 1 : 0);
      } else if (!va.numeric && !vb.numeric) {
        c = compare_varchar(va.text, vb.text);
      }
      if (c != 0) {
        return keys[k].desc ? c > 0 : c < 0;
      }
    }
    // Ties break on primary key ascending, as the task specifies.  Ordering by the key makes the
    // comparator deterministic, which std::sort and nth_element both require.
    return *a.pk < *b.pk;
  };
  if (!keys.empty()) {
    // A LIMIT with an ORDER BY only needs the first N; selecting a prefix keeps the work
    // proportional to what is actually returned.
    // LIMIT 0 is a valid limit that returns nothing; it must not be confused with the
    // "no LIMIT" sentinel (-1).  Only a non-empty prefix needs nth_element.
    if (stmt.limit >= 0 && static_cast<int64_t>(ranked.size()) > stmt.limit) {
      if (stmt.limit > 0) {
        std::nth_element(ranked.begin(), ranked.begin() + stmt.limit, ranked.end(), cmp_ranked);
      }
      ranked.resize(static_cast<size_t>(stmt.limit));
    }
    std::sort(ranked.begin(), ranked.end(), cmp_ranked);
    matched.clear();
    matched.reserve(ranked.size());
    for (const auto &r : ranked) {
      matched.push_back(r.pk);
    }
  } else if (stmt.limit >= 0 && static_cast<int64_t>(matched.size()) > stmt.limit) {
    matched.resize(static_cast<size_t>(stmt.limit));
  }
  Result r;
  for (const auto &it : items) {
    std::string name = it.alias;
    if (name.empty()) {
      name = it.is_l2 ? "distance" : it.column;
    }
    r.columns.push_back(name);
  }
  r.rows.reserve(matched.size());
  for (const auto *pk : matched) {
    Row row;
    if (!storage_.get_row(stmt.table, *pk, row)) {
      return make_error("io_error", "cannot read row");
    }
    std::vector<Value> out;
    out.reserve(items.size());
    for (const auto &it : items) {
      if (it.is_l2) {
        const int64_t pos = t->schema.column_pos(it.l2_column);
        const double dist = l2_distance(row[static_cast<size_t>(pos)].v, it.l2_query);
        out.push_back(Value::make_double(dist));
      } else {
        const int64_t pos = t->schema.column_pos(it.column);
        out.push_back(row[static_cast<size_t>(pos)]);
      }
    }
    r.rows.push_back(std::move(out));
  }
  return r;
}

void Engine::note_undo(const std::string &table, int64_t pk) {
  if (!in_tx_) {
    return;
  }
  // Keep only the earliest pre-image of each row: undoing a chain of writes has to restore the
  // value the row had when the transaction opened, not the value from two statements ago.
  for (const auto &u : undo_) {
    if (u.pk == pk && u.table == table) {
      return;
    }
  }
  UndoEntry entry;
  entry.table = table;
  entry.pk = pk;
  Row before;
  if (storage_.get_row(table, pk, before)) {
    entry.existed = true;
    entry.before = std::move(before);
  }
  undo_.push_back(std::move(entry));
}

Result Engine::exec_begin() {
  if (in_tx_) {
    // Nested transactions are explicitly outside the task's scope; report the agreed
    // unsupported error rather than a runtime-specific code.
    return make_error("unsupported_sql", "nested transactions are not supported");
  }
  // BEGIN allocates nothing proportional to the database; the undo log fills in as statements
  // touch rows.  This keeps the memory ceiling independent of table size -- the previous
  // full-catalogue copy doubled peak memory and was killed at 2 GiB on a 1.2M-row table.
  undo_.clear();
  tx_redo_.clear();
  in_tx_ = true;
  dirty_ = false;
  Result r;
  return r;
}

Result Engine::exec_commit() {
  if (!in_tx_) {
    Result r;
    return r;
  }
  // The transaction's writes become durable here: one fsynced redo record per statement,
  // written before the success response is produced.
  for (const std::string &payload : tx_redo_) {
    std::string err;
    if (!storage_.append_redo(payload, err)) {
      in_tx_ = false;
      undo_.clear();
      tx_redo_.clear();
      return make_error("io_error", err.c_str());
    }
  }
  tx_redo_.clear();
  undo_.clear();
  in_tx_ = false;
  dirty_ = false;
  needs_checkpoint_ = true;
  Result r;
  return r;
}

Result Engine::exec_rollback() {
  if (!in_tx_) {
    Result r;
    return r;
  }
  // Undo in reverse order so a row written by several statements ends at its original value.
  for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
    if (storage_.find_table(it->table) == nullptr) {
      continue;
    }
    std::string err;
    if (it->existed) {
      (void)storage_.put_row(it->table, it->pk, it->before, err);
    } else {
      (void)storage_.erase_row(it->table, it->pk, err);
    }
  }
  undo_.clear();
  tx_redo_.clear();
  in_tx_ = false;
  dirty_ = false;
  Result r;
  return r;
}

Result Engine::execute(const std::string &sql, const std::string &search_mode) {
  Stmt stmt;
  std::string parse_err;
  if (!parse_statement(sql, stmt, parse_err)) {
    // NULL, JOIN, subqueries, DISTINCT and nested transactions are outside the supported
    // subset, and the task requires unsupported input to be answered with the agreed error
    // instead of being given another meaning.  The parser tags those with an
    // "unsupported:" prefix; anything else really is a malformed statement.
    static const char kUnsupported[] = "unsupported:";
    if (parse_err.compare(0, sizeof(kUnsupported) - 1, kUnsupported) == 0) {
      return make_error("unsupported_sql", parse_err.c_str() + sizeof(kUnsupported) - 1);
    }
    return make_error("syntax_error", parse_err.c_str());
  }
  if (stmt.kind != Stmt::Kind::SELECT) {
    ++mutation_epoch_;
  }
  if (stmt.kind == Stmt::Kind::INSERT && !stmt.insert_columns.empty()) {
    std::string list_err;
    if (!normalize_insert_columns(stmt, list_err)) {
      return make_error("invalid_column", list_err.c_str());
    }
  }
  switch (stmt.kind) {
    case Stmt::Kind::UNSUPPORTED:
      return make_error("unsupported_sql", "statement is not supported");
    case Stmt::Kind::BEGIN:
      return exec_begin();
    case Stmt::Kind::COMMIT:
      return exec_commit();
    case Stmt::Kind::ROLLBACK:
      return exec_rollback();
    case Stmt::Kind::CREATE_TABLE: {
      Result r = exec_create_table(stmt);
      if (r.ok) {
        dirty_ = true;
        std::string payload;
        payload.push_back(static_cast<char>(REDO_CREATE_TABLE));
        const Table *t = storage_.find_table(stmt.table);
        if (t != nullptr) {
          encode_schema(payload, t->schema);
          std::string err;
          persist_or_buffer(payload, err);
        }
      }
      return r;
    }
    case Stmt::Kind::CREATE_VECTOR_INDEX: {
      Result r = exec_create_index(stmt);
      if (r.ok) {
        dirty_ = true;
        std::string payload;
        payload.push_back(static_cast<char>(REDO_CREATE_INDEX));
        put_str(payload, stmt.table);
        put_str(payload, stmt.index_name);
        std::string err;
        persist_or_buffer(payload, err);
      }
      return r;
    }
    case Stmt::Kind::INSERT: {
      Result r = exec_insert(stmt);
      if (r.ok) {
        dirty_ = true;
        std::string payload;
        payload.push_back(static_cast<char>(REDO_INSERT_ROWS));
        put_str(payload, stmt.table);
        put_u32(payload, static_cast<uint32_t>(stmt.rows.size()));
        const Table *t = storage_.find_table(stmt.table);
        if (t != nullptr) {
          for (const auto &raw : stmt.rows) {
            Row row;
            for (size_t k = 0; k < raw.size() && k < t->schema.columns.size(); ++k) {
              Value v;
              std::string ignored;
              coerce_for_column(t->schema.columns[k], raw[k], v, ignored);
              row.push_back(std::move(v));
            }
            encode_row(payload, row);
          }
          std::string err;
          persist_or_buffer(payload, err);
        }
      }
      return r;
    }
    case Stmt::Kind::UPDATE: {
      // The statement reports exactly the rows it rewrote, so the redo payload is built without
      // any full-table copy.
      std::vector<Row> changed;
      Result r = exec_update(stmt, changed);
      if (r.ok) {
        dirty_ = true;
        std::string payload;
        payload.push_back(static_cast<char>(REDO_UPDATE_ROWS));
        put_str(payload, stmt.table);
        put_u32(payload, static_cast<uint32_t>(changed.size()));
        for (const auto &row : changed) {
          encode_row(payload, row);
        }
        std::string err;
        persist_or_buffer(payload, err);
      }
      return r;
    }
    case Stmt::Kind::DELETE_ROWS: {
      std::vector<int64_t> removed;
      Result r = exec_delete(stmt, removed);
      if (r.ok) {
        dirty_ = true;
        std::string payload;
        payload.push_back(static_cast<char>(REDO_DELETE_ROWS));
        put_str(payload, stmt.table);
        put_u32(payload, static_cast<uint32_t>(removed.size()));
        for (int64_t pk : removed) {
          put_u64(payload, static_cast<uint64_t>(pk));
        }
        std::string err;
        persist_or_buffer(payload, err);
      }
      return r;
    }
    case Stmt::Kind::SELECT:
      return exec_select(stmt, search_mode);
  }
  return make_error("unsupported_sql", "statement is not supported");
}

}  // namespace mvdb
