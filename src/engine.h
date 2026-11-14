// Query execution, validation and transactions for MiniVectorDB.
#pragma once

#include <string>
#include <vector>

#include "sql.h"
#include "storage.h"

namespace mvdb {

struct Result {
  bool ok = true;
  std::string error_code;   // non-empty when ok == false
  std::string error_message;
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  bool has_affected_rows = false;
  int64_t affected_rows = 0;
};

// Replays one serialized redo payload onto the catalogue.  Used by recovery only.
bool apply_redo_payload(const char *payload, size_t len, std::map<std::string, Table> &tables);

class Engine {
public:
  Engine() = default;

  bool open(const std::string &dir, std::string &err);
  bool shutdown(std::string &err);

  // Executes one statement.  `search_mode` is "exact" or "ann".
  Result execute(const std::string &sql, const std::string &search_mode);

  // Records the redo payload for the given statement; only called for committed work.
  bool persist(const Stmt &stmt, const std::vector<std::string> &changed_tables, std::string &err);

private:
  Result exec_create_table(const Stmt &stmt);
  Result exec_create_index(const Stmt &stmt);
  Result exec_insert(const Stmt &stmt);
  // Reorders INSERT rows from an explicit column list into table-definition order.
  bool normalize_insert_columns(Stmt &stmt, std::string &err);
  // Builds (or reuses) the decoded-row cache for a table; false means the table is too large to
  // cache and the caller must stream.
  bool ensure_scan_cache(const Table *t);
  Result exec_select(const Stmt &stmt, const std::string &search_mode);
  Result exec_update(const Stmt &stmt, std::vector<Row> &changed);
  Result exec_delete(const Stmt &stmt, std::vector<int64_t> &removed);
  Result exec_begin();
  Result exec_commit();
  Result exec_rollback();

  // Writes a statement's redo payload now (autocommit) or buffers it for COMMIT.
  bool persist_or_buffer(const std::string &payload, std::string &err);

  // Builds the redo payload describing one already-applied operation.
  void encode_redo(const Stmt &stmt, std::string &out);

  Storage storage_;
  // Decoded-row cache for SELECT.  Every query used to re-read and re-decode each row from the
  // table file; a benchmark that runs thousands of TopK queries over the same table pays that
  // decode cost thousands of times.  The cache is rebuilt after any statement that can change
  // data (every non-SELECT bumps mutation_epoch_) and is skipped for tables whose decoded rows
  // would not fit the scan budget, so the "data larger than memory" case still streams.
  struct ScanCache {
    uint64_t epoch = 0;
    std::string table;
    std::vector<std::pair<int64_t, Row>> rows;
  };
  ScanCache scan_cache_;
  uint64_t mutation_epoch_ = 1;
  // Transaction state.  BEGIN records an undo log of the rows a statement is about to change
  // instead of copying the catalogue: a full copy doubles the peak memory of the largest
  // dataset, which is exactly where the task's "data larger than the memory budget" case lives
  // (measured: a 1.2M-row table at 2 GiB is killed by the kernel the moment BEGIN deep-copies
  // it).  One pre-image per touched row keeps rollback exact while costing memory proportional
  // to the transaction's own write set rather than to the table size.
  bool in_tx_ = false;
  struct UndoEntry {
    std::string table;
    int64_t pk = 0;
    bool existed = false;   // false: the row did not exist before this transaction
    Row before;             // pre-image, valid when existed
  };
  std::vector<UndoEntry> undo_;
  // Redo payloads of the statements executed inside the open transaction.  They are
  // written (and fsynced) only when COMMIT succeeds, so a crash before that point must
  // not resurrect any of them.
  std::vector<std::string> tx_redo_;
  bool dirty_ = false;         // uncommitted writes exist
  bool needs_checkpoint_ = false;

  // Captures the current on-disk state of one row before the transaction changes it.  Only the
  // first pre-image of a row is kept, so rewriting the same row inside a transaction still
  // rolls back to the original value.
  void note_undo(const std::string &table, int64_t pk);
};

}  // namespace mvdb
