#ifndef CLEAVE_PARSER_H
#define CLEAVE_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer *lex;
    Token current;
    int had_error;
} Parser;

void parser_init(Parser *p, Lexer *lex);

/* Parse a single top-level chain declaration. Returns NULL on parse error;
 * errors are reported to stderr. Caller does not free; the AST lives until
 * process exit. */
AstNode *parser_parse_chain(Parser *p);

int parser_had_error(const Parser *p);

#endif /* CLEAVE_PARSER_H */
