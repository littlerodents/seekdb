// Storage, recovery and transaction layer for MiniVectorDB.
//
// Design: the whole catalogue is an append-only redo log plus periodic compacted
// snapshots, all under /data.
//   * data/snapshot.bin  - a full image of all committed tables, written atomically
//                          (tmp + fsync + rename) on checkpoint and on clean shutdown.
//   * data/redo.log      - the log of committed statements since the last snapshot.
//                          Each record carries a payload length and a checksum; a torn
//                          tail is discarded on recovery, which is exactly what a crash
//                          should leave behind.
// Every mutation is applied to an in-memory copy first, so a failing statement can be
// dropped without touching the previous state, and BEGIN captures the pre-image used by
// ROLLBACK.
//
// Memory: rows live in the heap while the database is open.  Files are read back with a
// bounded reader so that recovery does not require a second full copy.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "sql.h"
#include "value.h"

namespace mvdb {

struct TableSchema {
  std::string name;
  std::vector<Stmt::ColumnDef> columns;
  int64_t pk_index = 0;          // position of the primary key column
  int64_t vector_index = -1;     // position of the vector column, -1 when absent
  std::vector<std::string> index_names;  // declared vector indexes (informational)

  int64_t column_pos(const std::string &name) const {
    for (size_t k = 0; k < columns.size(); ++k) {
      if (equal_ci(columns[k].name, name)) {
        return static_cast<int64_t>(k);
      }
    }
    return -1;
  }
  static bool equal_ci(const std::string &a, const std::string &b);
};

// One row keyed by its BIGINT primary key; ordered storage gives free ORDER BY id and
// deterministic tie-breaking.
using Row = std::vector<Value>;
using RowMap = std::map<int64_t, Row>;

struct Table {
  TableSchema schema;
  RowMap rows;
};

class Storage {
public:
  // Opens (or creates) the data directory and recovers committed state.
  // Returns false with err set when the directory is unusable or a file is corrupt
  // beyond the recoverable tail.
  bool open(const std::string &dir, std::string &err);
  // Checkpoints the current committed state and truncates the redo log.
  bool checkpoint(std::string &err);

  bool has_table(const std::string &name) const;
  Table *find_table(const std::string &name);
  const Table *find_table(const std::string &name) const;
  void put_table(Table &&table);
  void drop_table(const std::string &name);

  // Appends one committed statement to the redo log.  `payload` is the serialized
  // operation; the log write is fsynced before returning so an acknowledged commit is
  // durable.
  bool append_redo(const std::string &payload, std::string &err);

  const std::string &dir() const { return dir_; }
  std::vector<std::string> table_names() const;

private:
  bool recover(std::string &err);
  bool load_snapshot(std::string &err, bool &found);
  bool replay_redo(std::string &err);

  std::string dir_;
  std::string snapshot_path_;
  std::string redo_path_;
  int redo_fd_ = -1;
  std::map<std::string, Table> tables_;
};

// Serialization helpers shared with the redo log.  Kept simple and explicit so that a
// truncated or corrupt record is detected rather than misread.
// Applies one serialized redo payload to a catalogue image (used by recovery).
bool apply_redo_payload(const char *payload, size_t len, std::map<std::string, Table> &tables);

void encode_value(std::string &out, const Value &v);
bool decode_value(const char *&p, const char *end, Value &v);
void encode_schema(std::string &out, const TableSchema &s);
bool decode_schema(const char *&p, const char *end, TableSchema &s);
void encode_row(std::string &out, const Row &row);
bool decode_row(const char *&p, const char *end, Row &row, size_t column_count);

inline uint32_t crc32_of(const void *data, size_t len);

}  // namespace mvdb
