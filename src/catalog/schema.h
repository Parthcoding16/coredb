// =============================================================================
// schema.h — Column types, values, and fixed-width tuple serialization.
//
// CoreDB supports two types:
//   INT          -> 8-byte signed integer
//   VARCHAR(n)   -> 1 length byte + n bytes (n <= 255), zero padded
// Every column is fixed width, so every row of a table has the same byte size.
// That keeps the page layout simple (array of equal-size slots, no compaction)
// at the cost of wasting space on short strings — a deliberate trade-off.
// =============================================================================
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/config.h"

namespace coredb {

enum class TypeId : uint8_t { Int = 1, Varchar = 2 };

// A single SQL value. std::variant gives a type-safe tagged union: it is
// impossible to read an int out of a value that holds a string.
class Value {
public:
    Value() : v_(int64_t{0}) {}
    static Value integer(int64_t x) { Value v; v.v_ = x; return v; }
    static Value varchar(std::string s) { Value v; v.v_ = std::move(s); return v; }

    TypeId type() const { return std::holds_alternative<int64_t>(v_) ? TypeId::Int : TypeId::Varchar; }
    int64_t asInt() const { return std::get<int64_t>(v_); }
    const std::string& asString() const { return std::get<std::string>(v_); }

    // Three-way comparison; throws if the types differ.
    int compare(const Value& other) const;
    std::string toString() const;

private:
    std::variant<int64_t, std::string> v_;
};

using Tuple = std::vector<Value>;

struct Column {
    std::string name;
    TypeId      type = TypeId::Int;
    uint16_t    length = 0;  // Max characters, VARCHAR only.

    std::size_t byteSize() const { return type == TypeId::Int ? 8 : 1 + length; }
    std::string typeName() const {
        return type == TypeId::Int ? "INT" : "VARCHAR(" + std::to_string(length) + ")";
    }
};

class Schema {
public:
    Schema() = default;
    Schema(std::vector<Column> columns, int pk_column);

    const std::vector<Column>& columns() const { return columns_; }
    const Column& column(std::size_t i) const { return columns_[i]; }
    std::size_t numColumns() const { return columns_.size(); }

    // Index of the INT PRIMARY KEY column, or -1 if the table has none.
    int pkColumn() const { return pk_column_; }

    std::optional<std::size_t> indexOf(const std::string& name) const;

    // Bytes needed to store one tuple.
    std::size_t recordSize() const { return record_size_; }

    // Type-checks raw values against the schema and returns a valid Tuple.
    Tuple validate(const std::vector<Value>& values) const;

    void  serialize(const Tuple& tuple, char* out) const;
    Tuple deserialize(const char* in) const;

private:
    std::vector<Column> columns_;
    std::vector<std::size_t> offsets_;  // Byte offset of each column in a record.
    std::size_t record_size_ = 0;
    int pk_column_ = -1;
};

}  // namespace coredb
