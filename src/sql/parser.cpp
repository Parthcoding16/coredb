#include "sql/parser.h"

#include <cctype>
#include <set>

namespace coredb {

namespace {

const std::set<std::string> KEYWORDS = {
    "CREATE", "TABLE", "INT", "INTEGER", "VARCHAR", "PRIMARY", "KEY", "INSERT", "INTO",
    "VALUES", "SELECT", "FROM", "WHERE", "AND", "LIMIT", "DELETE", "UPDATE", "SET",
    "BEGIN", "COMMIT", "ROLLBACK", "EXPLAIN",
};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ---- Lexer -------------------------------------------------------------------------

std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    const std::size_t n = sql.size();
    while (i < n) {
        const char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        const std::size_t start = i;
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (i < n && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) ++i;
            std::string word = sql.substr(start, i - start);
            if (KEYWORDS.count(upper(word))) tokens.push_back({TokenKind::Keyword, upper(word), start});
            else tokens.push_back({TokenKind::Identifier, lower(word), start});
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
            tokens.push_back({TokenKind::Number, sql.substr(start, i - start), start});
        } else if (c == '\'') {
            // String literal; '' inside a string is an escaped single quote.
            std::string text;
            ++i;
            while (true) {
                if (i >= n) throw DbException("unterminated string starting at position " + std::to_string(start));
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') { text += '\''; i += 2; continue; }
                    ++i;
                    break;
                }
                text += sql[i++];
            }
            tokens.push_back({TokenKind::String, text, start});
        } else {
            // Two-character symbols first (longest match), then single ones.
            std::string two = sql.substr(i, 2);
            if (two == "<=" || two == ">=" || two == "!=" || two == "<>") {
                tokens.push_back({TokenKind::Symbol, two, start});
                i += 2;
            } else if (std::string("(),;*=<>+-").find(c) != std::string::npos) {
                tokens.push_back({TokenKind::Symbol, std::string(1, c), start});
                ++i;
            } else {
                throw DbException(std::string("unexpected character '") + c + "' at position " +
                                  std::to_string(start));
            }
        }
    }
    tokens.push_back({TokenKind::End, "", n});
    return tokens;
}

// ---- Token helpers ---------------------------------------------------------------------

bool Parser::isKeyword(const char* kw) const { return peek().kind == TokenKind::Keyword && peek().text == kw; }
bool Parser::isSymbol(const char* s) const { return peek().kind == TokenKind::Symbol && peek().text == s; }
bool Parser::acceptKeyword(const char* kw) { if (isKeyword(kw)) { ++pos_; return true; } return false; }
bool Parser::acceptSymbol(const char* s) { if (isSymbol(s)) { ++pos_; return true; } return false; }
void Parser::expectKeyword(const char* kw) { if (!acceptKeyword(kw)) fail(kw); }
void Parser::expectSymbol(const char* s) { if (!acceptSymbol(s)) fail(std::string("'") + s + "'"); }

void Parser::fail(const std::string& expected) const {
    const Token& t = peek();
    std::string got = t.kind == TokenKind::End ? "end of input" : "'" + t.text + "'";
    throw DbException("syntax error at position " + std::to_string(t.pos) + ": expected " +
                      expected + ", got " + got);
}

std::string Parser::parseIdentifier() {
    if (peek().kind != TokenKind::Identifier) fail("an identifier");
    return next().text;
}

int64_t Parser::parseNumber() {
    if (peek().kind != TokenKind::Number) fail("a number");
    try {
        return std::stoll(next().text);
    } catch (const std::out_of_range&) {
        throw DbException("number out of range");
    }
}

Value Parser::parseLiteral() {
    if (peek().kind == TokenKind::String) return Value::varchar(next().text);
    bool negative = acceptSymbol("-");
    int64_t v = parseNumber();
    return Value::integer(negative ? -v : v);
}

// ---- Grammar rules -----------------------------------------------------------------

Statement Parser::parse() {
    Statement stmt;
    if (isKeyword("CREATE")) stmt = parseCreate();
    else if (isKeyword("INSERT")) stmt = parseInsert();
    else if (isKeyword("SELECT")) stmt = parseSelect();
    else if (isKeyword("DELETE")) stmt = parseDelete();
    else if (isKeyword("UPDATE")) stmt = parseUpdate();
    else if (acceptKeyword("BEGIN")) stmt = BeginStmt{};
    else if (acceptKeyword("COMMIT")) stmt = CommitStmt{};
    else if (acceptKeyword("ROLLBACK")) stmt = RollbackStmt{};
    else if (acceptKeyword("EXPLAIN")) {
        if (isKeyword("SELECT")) stmt = ExplainStmt{parseSelect()};
        else if (isKeyword("DELETE")) stmt = ExplainStmt{parseDelete()};
        else if (isKeyword("UPDATE")) stmt = ExplainStmt{parseUpdate()};
        else fail("SELECT, DELETE or UPDATE after EXPLAIN");
    } else {
        fail("a statement");
    }
    acceptSymbol(";");
    if (peek().kind != TokenKind::End) fail("end of statement");
    return stmt;
}

CreateTableStmt Parser::parseCreate() {
    CreateTableStmt s;
    expectKeyword("CREATE");
    expectKeyword("TABLE");
    s.table = parseIdentifier();
    expectSymbol("(");
    do {
        ColumnDef col;
        col.name = parseIdentifier();
        if (acceptKeyword("INT") || acceptKeyword("INTEGER")) {
            col.type = TypeId::Int;
        } else if (acceptKeyword("VARCHAR")) {
            col.type = TypeId::Varchar;
            expectSymbol("(");
            int64_t len = parseNumber();
            if (len < 1 || len > 255) throw DbException("VARCHAR length must be between 1 and 255");
            col.length = static_cast<uint16_t>(len);
            expectSymbol(")");
        } else {
            fail("a type (INT or VARCHAR(n))");
        }
        if (acceptKeyword("PRIMARY")) {
            expectKeyword("KEY");
            col.primary_key = true;
        }
        s.columns.push_back(col);
    } while (acceptSymbol(","));
    expectSymbol(")");
    return s;
}

InsertStmt Parser::parseInsert() {
    InsertStmt s;
    expectKeyword("INSERT");
    expectKeyword("INTO");
    s.table = parseIdentifier();
    expectKeyword("VALUES");
    do {
        expectSymbol("(");
        std::vector<Value> row;
        do { row.push_back(parseLiteral()); } while (acceptSymbol(","));
        expectSymbol(")");
        s.rows.push_back(std::move(row));
    } while (acceptSymbol(","));
    return s;
}

SelectStmt Parser::parseSelect() {
    SelectStmt s;
    expectKeyword("SELECT");
    if (!acceptSymbol("*")) {
        do { s.columns.push_back(parseIdentifier()); } while (acceptSymbol(","));
    }
    expectKeyword("FROM");
    s.table = parseIdentifier();
    s.where = parseWhere();
    if (acceptKeyword("LIMIT")) s.limit = parseNumber();
    return s;
}

DeleteStmt Parser::parseDelete() {
    DeleteStmt s;
    expectKeyword("DELETE");
    expectKeyword("FROM");
    s.table = parseIdentifier();
    s.where = parseWhere();
    return s;
}

UpdateStmt Parser::parseUpdate() {
    UpdateStmt s;
    expectKeyword("UPDATE");
    s.table = parseIdentifier();
    expectKeyword("SET");
    do {
        Assignment a;
        a.column = parseIdentifier();
        expectSymbol("=");
        if (peek().kind == TokenKind::Identifier) {
            a.source_column = parseIdentifier();          // col = col + 5
            bool minus = acceptSymbol("-");
            if (!minus) expectSymbol("+");
            int64_t delta = parseNumber();
            a.value = Value::integer(minus ? -delta : delta);
        } else {
            a.value = parseLiteral();
        }
        s.assignments.push_back(std::move(a));
    } while (acceptSymbol(","));
    s.where = parseWhere();
    return s;
}

std::vector<Condition> Parser::parseWhere() {
    std::vector<Condition> conds;
    if (!acceptKeyword("WHERE")) return conds;
    do {
        Condition c;
        c.column = parseIdentifier();
        if (acceptSymbol("=")) c.op = CompareOp::Eq;
        else if (acceptSymbol("!=") || acceptSymbol("<>")) c.op = CompareOp::Ne;
        else if (acceptSymbol("<=")) c.op = CompareOp::Le;
        else if (acceptSymbol(">=")) c.op = CompareOp::Ge;
        else if (acceptSymbol("<")) c.op = CompareOp::Lt;
        else if (acceptSymbol(">")) c.op = CompareOp::Gt;
        else fail("a comparison operator");
        c.value = parseLiteral();
        conds.push_back(std::move(c));
    } while (acceptKeyword("AND"));
    return conds;
}

}  // namespace coredb
