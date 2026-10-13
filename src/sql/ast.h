// =============================================================================
// ast.h — Abstract syntax tree produced by the parser.
//
// A Statement is a std::variant over the supported statement structs, and the
// executor dispatches with std::visit. Compared with a class hierarchy plus
// virtual functions, this keeps the AST as plain data and makes the compiler
// check that every statement kind is handled.
// =============================================================================
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "catalog/schema.h"

namespace coredb {

enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };

// WHERE <column> <op> <literal>
struct Condition {
    std::string column;
    CompareOp op;
    Value value;
};

struct ColumnDef {
    std::string name;
    TypeId type;
    uint16_t length = 0;
    bool primary_key = false;
};

struct CreateTableStmt {
    std::string table;
    std::vector<ColumnDef> columns;
};

struct InsertStmt {
    std::string table;
    std::vector<std::vector<Value>> rows;
};

struct SelectStmt {
    std::string table;
    std::vector<std::string> columns;  // empty = SELECT *
    std::vector<Condition> where;      // conditions joined by AND
    std::optional<int64_t> limit;
};

struct DeleteStmt {
    std::string table;
    std::vector<Condition> where;
};

// SET <column> = <literal>   or   SET <column> = <column> (+|-) <int>
struct Assignment {
    std::string column;
    Value value;
    std::optional<std::string> source_column;  // set => value is a delta
};

struct UpdateStmt {
    std::string table;
    std::vector<Assignment> assignments;
    std::vector<Condition> where;
};

struct BeginStmt {};
struct CommitStmt {};
struct RollbackStmt {};

using DmlStatement = std::variant<SelectStmt, DeleteStmt, UpdateStmt>;

struct ExplainStmt {
    DmlStatement inner;
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, DeleteStmt, UpdateStmt,
                               BeginStmt, CommitStmt, RollbackStmt, ExplainStmt>;

}  // namespace coredb
