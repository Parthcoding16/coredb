// =============================================================================
// parser.h — SQL tokenizer and recursive-descent parser.
//
// Two stages, like a compiler front end:
//   1. Lexer  : characters -> tokens (keywords, identifiers, numbers, strings,
//               symbols). Identifiers and keywords are case-insensitive.
//   2. Parser : tokens -> AST. One function per grammar rule; each function
//               consumes the tokens of its rule and calls the functions of the
//               rules nested inside it (hence "recursive descent").
//
// Grammar (EBNF):
//   statement  := create | insert | select | delete | update
//               | BEGIN | COMMIT | ROLLBACK | EXPLAIN (select | delete | update)
//   create     := CREATE TABLE ident '(' coldef { ',' coldef } ')'
//   coldef     := ident ( INT | INTEGER | VARCHAR '(' number ')' ) [ PRIMARY KEY ]
//   insert     := INSERT INTO ident VALUES row { ',' row }
//   row        := '(' literal { ',' literal } ')'
//   select     := SELECT ( '*' | ident { ',' ident } ) FROM ident [ where ] [ LIMIT number ]
//   delete     := DELETE FROM ident [ where ]
//   update     := UPDATE ident SET assign { ',' assign } [ where ]
//   assign     := ident '=' ( literal | ident ( '+' | '-' ) number )
//   where      := WHERE cond { AND cond }
//   cond       := ident ( '=' | '!=' | '<>' | '<' | '<=' | '>' | '>=' ) literal
//   literal    := [ '-' ] number | 'string'
// =============================================================================
#pragma once

#include <string>
#include <vector>

#include "sql/ast.h"

namespace coredb {

enum class TokenKind { Identifier, Keyword, Number, String, Symbol, End };

struct Token {
    TokenKind kind;
    std::string text;  // keywords upper-cased, identifiers lower-cased
    std::size_t pos;   // offset in the input, for error messages
};

std::vector<Token> tokenize(const std::string& sql);

class Parser {
public:
    explicit Parser(const std::string& sql) : tokens_(tokenize(sql)) {}

    // Parses exactly one statement (an optional trailing ';' is allowed).
    Statement parse();

private:
    CreateTableStmt parseCreate();
    InsertStmt parseInsert();
    SelectStmt parseSelect();
    DeleteStmt parseDelete();
    UpdateStmt parseUpdate();
    std::vector<Condition> parseWhere();
    Value parseLiteral();
    std::string parseIdentifier();
    int64_t parseNumber();

    const Token& peek() const { return tokens_[pos_]; }
    const Token& next() { return tokens_[pos_++]; }
    bool isKeyword(const char* kw) const;
    bool isSymbol(const char* sym) const;
    bool acceptKeyword(const char* kw);
    bool acceptSymbol(const char* sym);
    void expectKeyword(const char* kw);
    void expectSymbol(const char* sym);
    [[noreturn]] void fail(const std::string& expected) const;

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

}  // namespace coredb
