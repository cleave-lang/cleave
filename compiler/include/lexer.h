#ifndef CLEAVE_LEXER_H
#define CLEAVE_LEXER_H

#include <stddef.h>

typedef enum {
    TK_EOF,
    TK_ERROR,

    /* identifiers (keywords are recognized via post-scan lookup) */
    TK_IDENT,

    /* literals */
    TK_NUMBER, /* decimal, hex (0x...), or binary (0b...) integer */
    TK_STRING, /* "..." with backslash escapes */
    TK_CHAR,   /* '.' with backslash escapes */

    /* keywords */
    TK_KW_CHAIN,
    TK_KW_MODULE,
    TK_KW_PROTOCOL,
    TK_KW_CONSENSUS,
    TK_KW_GAS,
    TK_KW_STATE,
    TK_KW_EXEC,
    TK_KW_DA,
    TK_KW_FN,
    TK_KW_EFFECT,
    TK_KW_SLASH_ON,
    TK_KW_IMPLEMENTS,
    TK_KW_WHEN,
    TK_KW_PENALTY,
    TK_KW_LET,
    TK_KW_IF,
    TK_KW_ELSE,
    TK_KW_MATCH,
    TK_KW_RETURN,
    TK_KW_TRUE,
    TK_KW_FALSE,
    TK_KW_NULL,

    /* single-character punctuation */
    TK_LBRACE,    /* { */
    TK_RBRACE,    /* } */
    TK_LPAREN,    /* ( */
    TK_RPAREN,    /* ) */
    TK_LBRACKET,  /* [ */
    TK_RBRACKET,  /* ] */
    TK_LT,        /* < */
    TK_GT,        /* > */
    TK_COMMA,     /* , */
    TK_COLON,     /* : */
    TK_SEMICOLON, /* ; */
    TK_ASSIGN,    /* = */
    TK_PLUS,      /* + */
    TK_MINUS,     /* - */
    TK_STAR,      /* * */
    TK_SLASH,     /* / */
    TK_BANG,      /* ! */
    TK_DOT,       /* . */
    TK_PIPE,      /* | */
    TK_PERCENT,   /* % */

    /* multi-character punctuation */
    TK_ARROW,     /* -> */
    TK_FAT_ARROW, /* => */
    TK_EQ,        /* == */
    TK_NEQ,       /* != */
    TK_LEQ,       /* <= */
    TK_GEQ,       /* >= */
    TK_AND,       /* && */
    TK_OR,        /* || */
    TK_DOTDOT,    /* .. */
    TK_COLONCOLON /* :: */
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start; /* points into the source buffer (not owned) */
    size_t length;
    size_t line;   /* 1-based */
    size_t column; /* 1-based */
} Token;

typedef struct {
    const char *source;     /* base of source buffer (not owned) */
    const char *cursor;     /* current scan position */
    const char *line_start; /* start of current line for column calc */
    size_t line;            /* 1-based */
} Lexer;

void lexer_init(Lexer *lex, const char *source);
Token lexer_next(Lexer *lex);
const char *token_kind_name(TokenKind kind);

#endif /* CLEAVE_LEXER_H */
