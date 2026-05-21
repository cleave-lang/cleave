#include <stddef.h>
#include <string.h>

#include "lexer.h"

/* ============== keyword table ============== */

struct keyword_entry {
    const char *text;
    size_t length;
    TokenKind kind;
};

/* Sorted by length first, then alphabetically, so the loop in
 * classify_ident below can short-circuit on length mismatch. */
static const struct keyword_entry KEYWORDS[] = {
    {"da",         2, TK_KW_DA},
    {"fn",         2, TK_KW_FN},
    {"if",         2, TK_KW_IF},
    {"gas",        3, TK_KW_GAS},
    {"let",        3, TK_KW_LET},
    {"null",       4, TK_KW_NULL},
    {"exec",       4, TK_KW_EXEC},
    {"else",       4, TK_KW_ELSE},
    {"true",       4, TK_KW_TRUE},
    {"when",       4, TK_KW_WHEN},
    {"chain",      5, TK_KW_CHAIN},
    {"false",      5, TK_KW_FALSE},
    {"match",      5, TK_KW_MATCH},
    {"state",      5, TK_KW_STATE},
    {"effect",     6, TK_KW_EFFECT},
    {"module",     6, TK_KW_MODULE},
    {"return",     6, TK_KW_RETURN},
    {"penalty",    7, TK_KW_PENALTY},
    {"slash_on",   8, TK_KW_SLASH_ON},
    {"protocol",   8, TK_KW_PROTOCOL},
    {"consensus",  9, TK_KW_CONSENSUS},
    {"implements", 10, TK_KW_IMPLEMENTS},
};

static const size_t N_KEYWORDS = sizeof(KEYWORDS) / sizeof(KEYWORDS[0]);

static TokenKind classify_ident(const char *start, size_t length) {
    for (size_t i = 0; i < N_KEYWORDS; ++i) {
        if (KEYWORDS[i].length == length &&
            memcmp(KEYWORDS[i].text, start, length) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TK_IDENT;
}

/* ============== character classification ============== */

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

/* ============== lexer state ============== */

static char peek(const Lexer *lex) {
    return *lex->cursor;
}

static char peek_at(const Lexer *lex, size_t offset) {
    /* Caller must ensure we don't read past the null terminator,
     * which is fine since strings are null-terminated and we stop
     * advancing at '\0'. */
    for (size_t i = 0; i < offset; ++i) {
        if (lex->cursor[i] == '\0') return '\0';
    }
    return lex->cursor[offset];
}

static void advance(Lexer *lex) {
    if (*lex->cursor == '\n') {
        lex->line++;
        lex->line_start = lex->cursor + 1;
    }
    lex->cursor++;
}

static size_t current_column(const Lexer *lex) {
    return (size_t)(lex->cursor - lex->line_start) + 1;
}

/* ============== whitespace + comments ============== */

static void skip_whitespace_and_comments(Lexer *lex) {
    for (;;) {
        char c = peek(lex);
        switch (c) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            advance(lex);
            break;
        case '/':
            if (peek_at(lex, 1) == '/') {
                /* line comment, consume to end of line */
                while (peek(lex) != '\0' && peek(lex) != '\n') advance(lex);
            } else if (peek_at(lex, 1) == '*') {
                /* block comment, consume until */ /*
                 * (we look for the closing pair) */
                advance(lex); /* consume '/' */
                advance(lex); /* consume '*' */
                while (peek(lex) != '\0') {
                    if (peek(lex) == '*' && peek_at(lex, 1) == '/') {
                        advance(lex);
                        advance(lex);
                        break;
                    }
                    advance(lex);
                }
            } else {
                return;
            }
            break;
        default:
            return;
        }
    }
}

/* ============== token builders ============== */

static Token make_token(TokenKind kind, const char *start, size_t length,
                        size_t line, size_t column) {
    Token tok;
    tok.kind = kind;
    tok.start = start;
    tok.length = length;
    tok.line = line;
    tok.column = column;
    return tok;
}

static Token scan_ident(Lexer *lex) {
    const char *start = lex->cursor;
    size_t line = lex->line;
    size_t column = current_column(lex);

    while (is_alnum(peek(lex))) advance(lex);

    size_t length = (size_t)(lex->cursor - start);
    return make_token(classify_ident(start, length), start, length, line, column);
}

static Token single(Lexer *lex, TokenKind kind) {
    const char *start = lex->cursor;
    size_t line = lex->line;
    size_t column = current_column(lex);
    advance(lex);
    return make_token(kind, start, 1, line, column);
}

static Token double_char(Lexer *lex, TokenKind kind) {
    const char *start = lex->cursor;
    size_t line = lex->line;
    size_t column = current_column(lex);
    advance(lex);
    advance(lex);
    return make_token(kind, start, 2, line, column);
}

/* ============== public API ============== */

void lexer_init(Lexer *lex, const char *source) {
    lex->source = source;
    lex->cursor = source;
    lex->line_start = source;
    lex->line = 1;
}

Token lexer_next(Lexer *lex) {
    skip_whitespace_and_comments(lex);

    char c = peek(lex);
    if (c == '\0') {
        return make_token(TK_EOF, lex->cursor, 0, lex->line, current_column(lex));
    }

    if (is_alpha(c)) {
        return scan_ident(lex);
    }

    /* multi-character punctuation first */
    char n = peek_at(lex, 1);
    if (c == '-' && n == '>') return double_char(lex, TK_ARROW);
    if (c == '=' && n == '>') return double_char(lex, TK_FAT_ARROW);
    if (c == '=' && n == '=') return double_char(lex, TK_EQ);
    if (c == '!' && n == '=') return double_char(lex, TK_NEQ);
    if (c == '<' && n == '=') return double_char(lex, TK_LEQ);
    if (c == '>' && n == '=') return double_char(lex, TK_GEQ);
    if (c == '&' && n == '&') return double_char(lex, TK_AND);
    if (c == '|' && n == '|') return double_char(lex, TK_OR);
    if (c == '.' && n == '.') return double_char(lex, TK_DOTDOT);

    /* single-character punctuation */
    switch (c) {
    case '{': return single(lex, TK_LBRACE);
    case '}': return single(lex, TK_RBRACE);
    case '(': return single(lex, TK_LPAREN);
    case ')': return single(lex, TK_RPAREN);
    case '[': return single(lex, TK_LBRACKET);
    case ']': return single(lex, TK_RBRACKET);
    case '<': return single(lex, TK_LT);
    case '>': return single(lex, TK_GT);
    case ',': return single(lex, TK_COMMA);
    case ':': return single(lex, TK_COLON);
    case ';': return single(lex, TK_SEMICOLON);
    case '=': return single(lex, TK_ASSIGN);
    case '+': return single(lex, TK_PLUS);
    case '-': return single(lex, TK_MINUS);
    case '*': return single(lex, TK_STAR);
    case '/': return single(lex, TK_SLASH);
    case '!': return single(lex, TK_BANG);
    case '.': return single(lex, TK_DOT);
    default: break;
    }

    /* unrecognized character; emit error token (length 1, advance past it) */
    Token err = make_token(TK_ERROR, lex->cursor, 1, lex->line, current_column(lex));
    advance(lex);
    return err;
}

/* ============== name table ============== */

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
    case TK_EOF:          return "EOF";
    case TK_ERROR:        return "ERROR";
    case TK_IDENT:        return "IDENT";
    case TK_KW_CHAIN:     return "KW_CHAIN";
    case TK_KW_MODULE:    return "KW_MODULE";
    case TK_KW_PROTOCOL:  return "KW_PROTOCOL";
    case TK_KW_CONSENSUS: return "KW_CONSENSUS";
    case TK_KW_GAS:       return "KW_GAS";
    case TK_KW_STATE:     return "KW_STATE";
    case TK_KW_EXEC:      return "KW_EXEC";
    case TK_KW_DA:        return "KW_DA";
    case TK_KW_FN:        return "KW_FN";
    case TK_KW_EFFECT:    return "KW_EFFECT";
    case TK_KW_SLASH_ON:  return "KW_SLASH_ON";
    case TK_KW_IMPLEMENTS:return "KW_IMPLEMENTS";
    case TK_KW_WHEN:      return "KW_WHEN";
    case TK_KW_PENALTY:   return "KW_PENALTY";
    case TK_KW_LET:       return "KW_LET";
    case TK_KW_IF:        return "KW_IF";
    case TK_KW_ELSE:      return "KW_ELSE";
    case TK_KW_MATCH:     return "KW_MATCH";
    case TK_KW_RETURN:    return "KW_RETURN";
    case TK_KW_TRUE:      return "KW_TRUE";
    case TK_KW_FALSE:     return "KW_FALSE";
    case TK_KW_NULL:      return "KW_NULL";
    case TK_LBRACE:       return "LBRACE";
    case TK_RBRACE:       return "RBRACE";
    case TK_LPAREN:       return "LPAREN";
    case TK_RPAREN:       return "RPAREN";
    case TK_LBRACKET:     return "LBRACKET";
    case TK_RBRACKET:     return "RBRACKET";
    case TK_LT:           return "LT";
    case TK_GT:           return "GT";
    case TK_COMMA:        return "COMMA";
    case TK_COLON:        return "COLON";
    case TK_SEMICOLON:    return "SEMICOLON";
    case TK_ASSIGN:       return "ASSIGN";
    case TK_PLUS:         return "PLUS";
    case TK_MINUS:        return "MINUS";
    case TK_STAR:         return "STAR";
    case TK_SLASH:        return "SLASH";
    case TK_BANG:         return "BANG";
    case TK_DOT:          return "DOT";
    case TK_ARROW:        return "ARROW";
    case TK_FAT_ARROW:    return "FAT_ARROW";
    case TK_EQ:           return "EQ";
    case TK_NEQ:          return "NEQ";
    case TK_LEQ:          return "LEQ";
    case TK_GEQ:          return "GEQ";
    case TK_AND:          return "AND";
    case TK_OR:           return "OR";
    case TK_DOTDOT:       return "DOTDOT";
    }
    return "UNKNOWN";
}
