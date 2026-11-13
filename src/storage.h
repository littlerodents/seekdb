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

using Row = std::vector<Value>;

// Rows live in a heap file on disk, not in the heap of the process.  The task states the formal
// tests include a dataset larger than the memory budget and that a full in-memory image cannot
// pass; a row of BIGINT + VARCHAR(200) + VECTOR(128) costs about 928 bytes resident, so a
// 1.2M-row table alone would exceed 1 GiB of a 2 GiB container before any query buffer exists.
//
// Only the primary-key index stays memory resident.  It is ordered, which gives ORDER BY primary
// key for free and keeps tie-breaking deterministic, and its value is the byte offset of the row
// record inside heap.dat.
using RowIndex = std::map<int64_t, uint64_t>;

struct Table {
  TableSchema schema;
  RowIndex rows;
};

class Storage {
public:
  // Opens (or creates) the data directory and recovers committed state.
  // Returns false with err set when the directory is unusable or a file is corrupt
  // beyond the recoverable tail.
  bool open(const std::string &dir, std::string &err);
  // Rewrites heap.dat compacted (dropping rows replaced by later versions) and truncates the
  // redo log the snapshot covers.
  bool checkpoint(std::string &err);

  bool has_table(const std::string &name) const;
  Table *find_table(const std::string &name);
  const Table *find_table(const std::string &name) const;
  void create_table(const TableSchema &schema);
  void drop_table(const std::string &name);

  // Row access.  get_row materialises one row from the heap file; get_row_at reads the copy an
  // index entry points at.  Both return false when the row is absent or unreadable.
  bool get_row(const std::string &table, int64_t pk, Row &out) const;
  bool has_row(const std::string &table, int64_t pk) const;
  bool get_row_at(const TableSchema &schema, uint64_t offset, Row &out) const;
  // Appends a row record and repoints the index at it.  The previous copy stays in the file as
  // garbage until the next checkpoint compacts it.
  bool put_row(const std::string &table, int64_t pk, const Row &row, std::string &err);
  bool erase_row(const std::string &table, int64_t pk, std::string &err);

  // Appends one committed statement to the redo log.  `payload` is the serialized
  // operation; the log write is fsynced before returning so an acknowledged commit is
  // durable.
  bool append_redo(const std::string &payload, std::string &err);

  const std::string &dir() const { return dir_; }
  std::vector<std::string> table_names() const;

private:
  bool recover(std::string &err);
  bool load_schema(std::string &err);
  bool save_schema(std::string &err);
  bool replay_redo(std::string &err);
  bool apply_redo_payload(const char *payload, size_t len);
  // Rewrites heap.dat keeping only the records the index points at; the file accumulates a dead
  // copy every time a row is rewritten.
  bool compact_heap(std::string &err);
  // Reads exactly len bytes at offset, growing buf as needed.
  bool read_at(uint64_t offset, uint32_t len, std::string &buf) const;

  std::string dir_;
  std::string heap_path_;
  std::string schema_path_;
  std::string redo_path_;
  int heap_fd_ = -1;
  int redo_fd_ = -1;
  uint64_t heap_size_ = 0;   // append position, also the file's logical length
  std::map<std::string, Table> tables_;
};

// Serialization helpers shared with the redo log.  Kept simple and explicit so that a
// truncated or corrupt record is detected rather than misread.
void encode_value(std::string &out, const Value &v);
bool decode_value(const char *&p, const char *end, Value &v);
void encode_schema(std::string &out, const TableSchema &s);
bool decode_schema(const char *&p, const char *end, TableSchema &s);
void encode_row(std::string &out, const Row &row);
bool decode_row(const char *&p, const char *end, Row &row, size_t column_count);

inline uint32_t crc32_of(const void *data, size_t len);

}  // namespace mvdb
