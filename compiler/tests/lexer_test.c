#include "test.h"
#include "lexer.h"

/* ============== helpers ============== */

static void check_next(Lexer *lex, TokenKind expected_kind,
                       const char *expected_text) {
    Token t = lexer_next(lex);
    ASSERT_EQ_INT(t.kind, expected_kind);
    ASSERT_EQ_STR_LEN(t.start, t.length, expected_text);
}

/* ============== tests ============== */

TEST(test_eof_on_empty_input) {
    Lexer lex;
    lexer_init(&lex, "");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_EOF);
}

TEST(test_eof_after_whitespace_only) {
    Lexer lex;
    lexer_init(&lex, "   \n\t  ");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_EOF);
}

TEST(test_single_keyword) {
    Lexer lex;
    lexer_init(&lex, "chain");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_KW_CHAIN);
    ASSERT_EQ_STR_LEN(t.start, t.length, "chain");
}

TEST(test_all_keywords) {
    struct { const char *text; TokenKind kind; } cases[] = {
        {"chain",      TK_KW_CHAIN},
        {"module",     TK_KW_MODULE},
        {"protocol",   TK_KW_PROTOCOL},
        {"consensus",  TK_KW_CONSENSUS},
        {"gas",        TK_KW_GAS},
        {"state",      TK_KW_STATE},
        {"exec",       TK_KW_EXEC},
        {"da",         TK_KW_DA},
        {"fn",         TK_KW_FN},
        {"effect",     TK_KW_EFFECT},
        {"slash_on",   TK_KW_SLASH_ON},
        {"implements", TK_KW_IMPLEMENTS},
        {"when",       TK_KW_WHEN},
        {"penalty",    TK_KW_PENALTY},
        {"let",        TK_KW_LET},
        {"if",         TK_KW_IF},
        {"else",       TK_KW_ELSE},
        {"match",      TK_KW_MATCH},
        {"return",     TK_KW_RETURN},
        {"true",       TK_KW_TRUE},
        {"false",      TK_KW_FALSE},
        {"null",       TK_KW_NULL},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; ++i) {
        Lexer lex;
        lexer_init(&lex, cases[i].text);
        Token t = lexer_next(&lex);
        tests_run++;
        if (t.kind != cases[i].kind) {
            FAIL_AT("keyword '%s' lexed as kind %d, expected %d",
                    cases[i].text, (int)t.kind, (int)cases[i].kind);
        }
    }
}

TEST(test_identifiers) {
    Lexer lex;
    lexer_init(&lex, "MyChain Tendermint BLS12_381 _underscore foo123");
    check_next(&lex, TK_IDENT, "MyChain");
    check_next(&lex, TK_IDENT, "Tendermint");
    check_next(&lex, TK_IDENT, "BLS12_381");
    check_next(&lex, TK_IDENT, "_underscore");
    check_next(&lex, TK_IDENT, "foo123");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_EOF);
}

TEST(test_keyword_vs_identifier_disambiguation) {
    /* None of these are the 'chain' keyword: they have extra chars. */
    Lexer lex;
    lexer_init(&lex, "chains chained chain_");
    Token t;
    t = lexer_next(&lex); ASSERT_EQ_INT(t.kind, TK_IDENT);
    t = lexer_next(&lex); ASSERT_EQ_INT(t.kind, TK_IDENT);
    t = lexer_next(&lex); ASSERT_EQ_INT(t.kind, TK_IDENT);
}

TEST(test_single_char_punctuation) {
    struct { char c; TokenKind kind; } cases[] = {
        {'{', TK_LBRACE},     {'}', TK_RBRACE},
        {'(', TK_LPAREN},     {')', TK_RPAREN},
        {'[', TK_LBRACKET},   {']', TK_RBRACKET},
        {'<', TK_LT},         {'>', TK_GT},
        {',', TK_COMMA},      {':', TK_COLON},
        {';', TK_SEMICOLON},  {'=', TK_ASSIGN},
        {'+', TK_PLUS},       {'-', TK_MINUS},
        {'*', TK_STAR},       {'/', TK_SLASH},
        {'!', TK_BANG},       {'.', TK_DOT},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; ++i) {
        char buf[2] = {cases[i].c, '\0'};
        Lexer lex;
        lexer_init(&lex, buf);
        Token t = lexer_next(&lex);
        tests_run++;
        if (t.kind != cases[i].kind) {
            FAIL_AT("char '%c' lexed as kind %d, expected %d",
                    cases[i].c, (int)t.kind, (int)cases[i].kind);
        }
    }
}

TEST(test_multi_char_punctuation) {
    struct { const char *text; TokenKind kind; } cases[] = {
        {"->", TK_ARROW},     {"=>", TK_FAT_ARROW},
        {"==", TK_EQ},        {"!=", TK_NEQ},
        {"<=", TK_LEQ},       {">=", TK_GEQ},
        {"&&", TK_AND},       {"||", TK_OR},
        {"..", TK_DOTDOT},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; ++i) {
        Lexer lex;
        lexer_init(&lex, cases[i].text);
        Token t = lexer_next(&lex);
        tests_run++;
        if (t.kind != cases[i].kind) {
            FAIL_AT("'%s' lexed as kind %d, expected %d",
                    cases[i].text, (int)t.kind, (int)cases[i].kind);
        }
        Token e = lexer_next(&lex);
        ASSERT_EQ_INT(e.kind, TK_EOF);
    }
}

TEST(test_line_comments_skipped) {
    Lexer lex;
    lexer_init(&lex, "// this is a comment\nchain");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_KW_CHAIN);
    ASSERT_EQ_INT(t.line, 2);
}

TEST(test_block_comments_skipped) {
    Lexer lex;
    lexer_init(&lex, "/* block\ncomment */chain");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_KW_CHAIN);
    ASSERT_EQ_INT(t.line, 2);
}

TEST(test_whitespace_skipped) {
    Lexer lex;
    lexer_init(&lex, "   \t  chain  \n  module");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_KW_CHAIN);
    t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_KW_MODULE);
    ASSERT_EQ_INT(t.line, 2);
}

TEST(test_line_column_tracking) {
    Lexer lex;
    lexer_init(&lex, "chain\nMyChain");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 1);
    t = lexer_next(&lex);
    ASSERT_EQ_INT(t.line, 2);
    ASSERT_EQ_INT(t.column, 1);
}

TEST(test_arrow_versus_minus) {
    /* "->" is one ARROW token, "- >" is two tokens (MINUS, GT). */
    Lexer lex;
    lexer_init(&lex, "->");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_ARROW);

    lexer_init(&lex, "- >");
    t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_MINUS);
    t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_GT);
}

TEST(test_error_on_unknown_char) {
    Lexer lex;
    lexer_init(&lex, "@ chain");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_ERROR);
    /* Lexer should still recover and read the next token. */
    t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_KW_CHAIN);
}

TEST(test_chain_manifest_smoke) {
    const char *src = "chain MyChain {\n"
                      "    consensus: Tendermint\n"
                      "}";
    Lexer lex;
    lexer_init(&lex, src);
    check_next(&lex, TK_KW_CHAIN, "chain");
    check_next(&lex, TK_IDENT, "MyChain");
    check_next(&lex, TK_LBRACE, "{");
    check_next(&lex, TK_KW_CONSENSUS, "consensus");
    check_next(&lex, TK_COLON, ":");
    check_next(&lex, TK_IDENT, "Tendermint");
    check_next(&lex, TK_RBRACE, "}");
    Token t = lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TK_EOF);
}

int main(void) {
    RUN(test_eof_on_empty_input);
    RUN(test_eof_after_whitespace_only);
    RUN(test_single_keyword);
    RUN(test_all_keywords);
    RUN(test_identifiers);
    RUN(test_keyword_vs_identifier_disambiguation);
    RUN(test_single_char_punctuation);
    RUN(test_multi_char_punctuation);
    RUN(test_line_comments_skipped);
    RUN(test_block_comments_skipped);
    RUN(test_whitespace_skipped);
    RUN(test_line_column_tracking);
    RUN(test_arrow_versus_minus);
    RUN(test_error_on_unknown_char);
    RUN(test_chain_manifest_smoke);
    REPORT();
}
