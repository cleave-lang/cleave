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

/* Parse a full program: a sequence of top-level chain, module, and protocol
 * declarations. Returns a synthetic AST_MODULE_DECL node with name "" whose
 * items hold the parsed top-level declarations, or NULL on parse error.
 * This is the preferred entry point for compiling a source file. */
AstNode *parser_parse_program(Parser *p);

/* Parse a single expression. Useful for tests that exercise the expression
 * grammar in isolation. Returns NULL on parse error. */
AstNode *parser_parse_expression(Parser *p);

int parser_had_error(const Parser *p);

#endif /* CLEAVE_PARSER_H */
