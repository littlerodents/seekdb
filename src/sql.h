// Minimal SQL lexer and statement shapes for MiniVectorDB.
//
// The supported subset is fixed by the task: CREATE TABLE, INSERT, SELECT (with
// WHERE / ORDER BY / LIMIT), UPDATE, DELETE, CREATE VECTOR INDEX and the transaction
// statements.  Anything else has to be reported as unsupported_sql rather than being
// silently reinterpreted, so parsing produces a discriminated statement and the caller
// rejects the rest.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "value.h"

namespace mvdb {

enum class TokType { IDENT, NUMBER, STRING, PUNCT, END };

struct Token {
  TokType type = TokType::END;
  std::string text;   // IDENT: as written; STRING: decoded payload; NUMBER: raw digits
  int64_t from = 0;   // byte offset of the token in the source
  int64_t to = 0;
};

// Tokenizer for the SQL subset.  Double-quoted and single-quoted literals are both
// accepted for strings; '' and "" escape the quote character.
class Lexer {
public:
  explicit Lexer(const std::string &sql) : sql_(sql) {}
  bool next(Token &out);
  void reset() { pos_ = 0; }

private:
  const std::string &sql_;
  size_t pos_ = 0;
};

inline bool ident_start(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool ident_body(unsigned char c) {
  return ident_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '$';
}
inline char upper_of(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}
inline std::string to_upper(const std::string &s) {
  std::string r(s);
  for (char &c : r) {
    c = upper_of(c);
  }
  return r;
}

inline bool Lexer::next(Token &out) {
  while (pos_ < sql_.size() && (sql_[pos_] == ' ' || sql_[pos_] == '\t' ||
                                sql_[pos_] == '\n' || sql_[pos_] == '\r')) {
    ++pos_;
  }
  out = Token();
  if (pos_ >= sql_.size()) {
    out.type = TokType::END;
    out.from = out.to = static_cast<int64_t>(pos_);
    return false;
  }
  const size_t begin = pos_;
  const unsigned char c = static_cast<unsigned char>(sql_[pos_]);
  if (ident_start(c)) {
    while (pos_ < sql_.size() && ident_body(static_cast<unsigned char>(sql_[pos_]))) {
      ++pos_;
    }
    out.type = TokType::IDENT;
    out.text = sql_.substr(begin, pos_ - begin);
  } else if ((c >= '0' && c <= '9') ||
             (c == '-' && pos_ + 1 < sql_.size() && sql_[pos_ + 1] >= '0' &&
              sql_[pos_ + 1] <= '9')) {
    if (sql_[pos_] == '-') {
      ++pos_;
    }
    while (pos_ < sql_.size() && sql_[pos_] >= '0' && sql_[pos_] <= '9') {
      ++pos_;
    }
    out.type = TokType::NUMBER;
    out.text = sql_.substr(begin, pos_ - begin);
  } else if (c == '\'' || c == '"') {
    const char quote = static_cast<char>(c);
    ++pos_;
    std::string decoded;
    bool closed = false;
    while (pos_ < sql_.size()) {
      if (sql_[pos_] == quote) {
        if (pos_ + 1 < sql_.size() && sql_[pos_ + 1] == quote) {
          decoded.push_back(quote);
          pos_ += 2;
          continue;
        }
        ++pos_;
        closed = true;
        break;
      }
      if (sql_[pos_] == '\\' && pos_ + 1 < sql_.size()) {
        const char esc = sql_[pos_ + 1];
        switch (esc) {
          case 'n': decoded.push_back('\n'); break;
          case 't': decoded.push_back('\t'); break;
          case 'r': decoded.push_back('\r'); break;
          case '0': decoded.push_back('\0'); break;
          // MySQL keeps an unknown escape's character; "\'" becomes ' and "\\" becomes \.
          default: decoded.push_back(esc); break;
        }
        pos_ += 2;
        continue;
      }
      decoded.push_back(sql_[pos_]);
      ++pos_;
    }
    (void)closed;
    out.type = TokType::STRING;
    out.text = std::move(decoded);
  } else {
    out.type = TokType::PUNCT;
    out.text = std::string(1, static_cast<char>(c));
    ++pos_;
  }
  out.from = static_cast<int64_t>(begin);
  out.to = static_cast<int64_t>(pos_);
  return true;
}

// ---------- statement model ----------

enum class CmpOp { EQ, LT, LE, GT, GE };

struct Condition {
  std::string column;
  CmpOp op = CmpOp::EQ;
  Value literal;
};

struct WhereClause {
  bool present = false;
  std::vector<Condition> conds;  // joined with AND
};

struct OrderItem {
  std::string column;   // empty when the term is an L2_DISTANCE expression
  std::string alias;    // set when ordering by a projected alias
  bool desc = false;
  // ORDER BY L2_DISTANCE(col, '[...]'): the distance is computed for sorting only, with no
  // matching projection required.  This is the form the task's vector search section uses.
  bool is_l2 = false;
  std::string l2_column;
  std::vector<float> l2_query;
};

struct SelectItem {
  bool star = false;
  std::string column;
  std::string alias;
  // L2_DISTANCE(col, '[...]')
  bool is_l2 = false;
  std::string l2_column;
  std::vector<float> l2_query;
  bool l2_valid = false;
  // COUNT(*) / COUNT(col): a single-row aggregate, which is the only aggregate the task allows.
  bool is_count = false;
  bool count_star = false;
  std::string count_column;
};

struct Stmt {
  enum class Kind {
    CREATE_TABLE,
    INSERT,
    SELECT,
    UPDATE,
    DELETE_ROWS,
    CREATE_VECTOR_INDEX,
    BEGIN,
    COMMIT,
    ROLLBACK,
    UNSUPPORTED,
  };
  Kind kind = Kind::UNSUPPORTED;

  // CREATE TABLE
  std::string table;
  struct ColumnDef {
    std::string name;
    ColType type = ColType::BIGINT;
    int64_t length = 0;       // VARCHAR(n) characters, VECTOR(n) dimensions
    bool primary_key = false;
  };
  std::vector<ColumnDef> columns;

  // INSERT
  std::vector<std::vector<Value>> rows;

  // SELECT
  std::vector<SelectItem> select_items;
  WhereClause where;
  std::vector<OrderItem> order_by;
  int64_t limit = -1;

  // UPDATE
  struct Assignment {
    std::string column;
    Value literal;
  };
  std::vector<Assignment> assignments;

  // DELETE / SELECT share the WHERE clause.
  // CREATE VECTOR INDEX
  std::string index_name;
  std::string index_column;

  std::string original;  // the statement text as received
};

// Parses one statement.  On success stmt.kind is set; unsupported syntax yields
// Kind::UNSUPPORTED, and hard syntax errors yield false with err set.
bool parse_statement(const std::string &sql, Stmt &stmt, std::string &err);

// Parses a VECTOR literal such as '[1, 2.5, -3]'.  Rejects NaN, infinity and values
// that do not fit in float32.
bool parse_vector_literal(const std::string &text, std::vector<float> &out, std::string &err);

}  // namespace mvdb
