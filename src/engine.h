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
  Result exec_select(const Stmt &stmt, const std::string &search_mode);
  Result exec_update(const Stmt &stmt);
  Result exec_delete(const Stmt &stmt);
  Result exec_begin();
  Result exec_commit();
  Result exec_rollback();

  // Writes a statement's redo payload now (autocommit) or buffers it for COMMIT.
  bool persist_or_buffer(const std::string &payload, std::string &err);

  // Builds the redo payload describing one already-applied operation.
  void encode_redo(const Stmt &stmt, std::string &out);

  Storage storage_;
  // Transaction state: a full copy of the catalogue taken at BEGIN.  The data sets in
  // this task are small enough that copy-on-begin is both simple and fast, and it makes
  // rollback exact by construction.
  bool in_tx_ = false;
  std::map<std::string, Table> tx_snapshot_;
  // Redo payloads of the statements executed inside the open transaction.  They are
  // written (and fsynced) only when COMMIT succeeds, so a crash before that point must
  // not resurrect any of them.
  std::vector<std::string> tx_redo_;
  bool dirty_ = false;         // uncommitted writes exist
  bool needs_checkpoint_ = false;
};

}  // namespace mvdb
