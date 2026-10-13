#include "catalog/schema.h"

#include <cstring>

namespace coredb {

int Value::compare(const Value& other) const {
    if (type() != other.type()) throw DbException("type mismatch: cannot compare INT with VARCHAR");
    if (type() == TypeId::Int) {
        int64_t a = asInt(), b = other.asInt();
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    int c = asString().compare(other.asString());
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

std::string Value::toString() const {
    return type() == TypeId::Int ? std::to_string(asInt()) : asString();
}

Schema::Schema(std::vector<Column> columns, int pk_column)
    : columns_(std::move(columns)), pk_column_(pk_column) {
    for (const Column& c : columns_) {
        offsets_.push_back(record_size_);
        record_size_ += c.byteSize();
    }
}

std::optional<std::size_t> Schema::indexOf(const std::string& name) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name == name) return i;
    }
    return std::nullopt;
}

Tuple Schema::validate(const std::vector<Value>& values) const {
    if (values.size() != columns_.size()) {
        throw DbException("expected " + std::to_string(columns_.size()) + " values, got " +
                          std::to_string(values.size()));
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        const Column& col = columns_[i];
        if (values[i].type() != col.type) {
            throw DbException("column '" + col.name + "' expects " + col.typeName());
        }
        if (col.type == TypeId::Varchar && values[i].asString().size() > col.length) {
            throw DbException("value too long for column '" + col.name + "' (" + col.typeName() + ")");
        }
    }
    return values;
}

void Schema::serialize(const Tuple& tuple, char* out) const {
    std::memset(out, 0, record_size_);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        char* p = out + offsets_[i];
        if (columns_[i].type == TypeId::Int) {
            int64_t v = tuple[i].asInt();
            std::memcpy(p, &v, sizeof(v));
        } else {
            const std::string& s = tuple[i].asString();
            p[0] = static_cast<char>(s.size());
            std::memcpy(p + 1, s.data(), s.size());
        }
    }
}

Tuple Schema::deserialize(const char* in) const {
    Tuple tuple;
    tuple.reserve(columns_.size());
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const char* p = in + offsets_[i];
        if (columns_[i].type == TypeId::Int) {
            int64_t v;
            std::memcpy(&v, p, sizeof(v));
            tuple.push_back(Value::integer(v));
        } else {
            auto len = static_cast<uint8_t>(p[0]);
            tuple.push_back(Value::varchar(std::string(p + 1, len)));
        }
    }
    return tuple;
}

}  // namespace coredb
