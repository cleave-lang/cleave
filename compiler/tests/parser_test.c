/* open_memstream requires POSIX.1-2008 feature visibility on glibc. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"

/* ============== helpers ============== */

/* Redirect stderr to a buffer for the duration of a block of parsing so error
 * tests don't pollute the test output. */
static FILE *stderr_to_buf(char **buf, size_t *size) {
    FILE *new_stderr = open_memstream(buf, size);
    if (!new_stderr) return NULL;
    fflush(stderr);
    return new_stderr;
}

/* Parse a source string into a chain AST. Returns the root node (or NULL).
 * stderr from the parse is captured into *err_buf (caller frees). */
static AstNode *parse_source(const char *src, char **err_buf) {
    size_t err_size = 0;
    FILE *captured = stderr_to_buf(err_buf, &err_size);
    FILE *saved_stderr = stderr;
    if (captured) stderr = captured;

    Lexer lex;
    lexer_init(&lex, src);

    Parser p;
    parser_init(&p, &lex);

    AstNode *root = parser_parse_chain(&p);

    if (captured) {
        fflush(captured);
        fclose(captured);
        stderr = saved_stderr;
    }

    if (p.had_error) return NULL;
    return root;
}

/* ============== success-path tests ============== */

TEST(test_parse_minimal_chain_block) {
    const char *src = "chain MyChain { consensus: Tendermint }";
    char *err = NULL;
    AstNode *root = parse_source(src, &err);
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->kind, AST_CHAIN_DECL);
    ASSERT_EQ_STR_LEN(root->as.chain.name.start, root->as.chain.name.length, "MyChain");
    ASSERT_EQ_INT(root->as.chain.n_assignments, 1);
    AstNode *sub = root->as.chain.assignments[0];
    ASSERT_EQ_INT(sub->kind, AST_SUBSYSTEM_ASSIGN);
    ASSERT_EQ_STR_LEN(sub->as.subsystem.key.start, sub->as.subsystem.key.length,
                      "consensus");
    ASSERT_EQ_INT(sub->as.subsystem.value->kind, AST_TYPE_NAMED);
    free(err);
}

TEST(test_parse_chain_with_generic_args_and_kwargs) {
    const char *src =
        "chain MyChain { consensus: Tendermint<sig=BLS12_381, finality=Deterministic> }";
    char *err = NULL;
    AstNode *root = parse_source(src, &err);
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->kind, AST_CHAIN_DECL);

    AstNode *value = root->as.chain.assignments[0]->as.subsystem.value;
    ASSERT_EQ_INT(value->kind, AST_TYPE_GENERIC);
    ASSERT_EQ_STR_LEN(value->as.type_generic.name.start,
                      value->as.type_generic.name.length, "Tendermint");
    ASSERT_EQ_INT(value->as.type_generic.n_args, 2);

    AstNode *first = value->as.type_generic.args[0];
    ASSERT_EQ_STR_LEN(first->as.type_param.key.start,
                      first->as.type_param.key.length, "sig");
    ASSERT_EQ_INT(first->as.type_param.value->kind, AST_EXPR_IDENT);
    ASSERT_EQ_STR_LEN(first->as.type_param.value->as.ident.name.start,
                      first->as.type_param.value->as.ident.name.length, "BLS12_381");
    free(err);
}

TEST(test_parse_chain_with_set_literal_arg) {
    /* gas: Multidim<{cpu, storage, witness}> */
    const char *src = "chain C { gas: Multidim<{cpu, storage, witness}> }";
    char *err = NULL;
    AstNode *root = parse_source(src, &err);
    ASSERT(root != NULL);

    AstNode *value = root->as.chain.assignments[0]->as.subsystem.value;
    ASSERT_EQ_INT(value->kind, AST_TYPE_GENERIC);
    ASSERT_EQ_INT(value->as.type_generic.n_args, 1);

    /* The single arg is a TypeParam whose value is an anonymous TypeGeneric. */
    AstNode *outer_param = value->as.type_generic.args[0];
    ASSERT_EQ_INT(outer_param->kind, AST_TYPE_PARAM);
    AstNode *set = outer_param->as.type_param.value;
    ASSERT_EQ_INT(set->kind, AST_TYPE_GENERIC);
    ASSERT_EQ_INT(set->as.type_generic.name.length, 0); /* anonymous */
    ASSERT_EQ_INT(set->as.type_generic.n_args, 3);
    free(err);
}

TEST(test_parse_chain_with_all_four_subsystems) {
    const char *src =
        "chain Big {"
        "  consensus: Tendermint"
        "  gas:       Multidim<{cpu, storage, witness}>"
        "  state:     SparseMerkle<key=u256, hash=Blake3>"
        "  exec:      WasmVM<deterministic>"
        "}";
    char *err = NULL;
    AstNode *root = parse_source(src, &err);
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->as.chain.n_assignments, 4);
    /* Spot-check key names. */
    ASSERT_EQ_STR_LEN(root->as.chain.assignments[0]->as.subsystem.key.start,
                      root->as.chain.assignments[0]->as.subsystem.key.length,
                      "consensus");
    ASSERT_EQ_STR_LEN(root->as.chain.assignments[3]->as.subsystem.key.start,
                      root->as.chain.assignments[3]->as.subsystem.key.length, "exec");
    free(err);
}

TEST(test_parse_empty_chain_block) {
    const char *src = "chain Empty { }";
    char *err = NULL;
    AstNode *root = parse_source(src, &err);
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->as.chain.n_assignments, 0);
    free(err);
}

/* ============== error tests ============== */

TEST(test_error_missing_chain_keyword) {
    char *err = NULL;
    AstNode *root = parse_source("MyChain { consensus: Tendermint }", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "expected 'chain'") != NULL);
    free(err);
}

TEST(test_error_missing_chain_name) {
    char *err = NULL;
    AstNode *root = parse_source("chain { consensus: Tendermint }", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "expected a chain name") != NULL);
    free(err);
}

TEST(test_error_missing_open_brace) {
    char *err = NULL;
    AstNode *root = parse_source("chain MyChain consensus: Tendermint }", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "'{") != NULL);
    free(err);
}

TEST(test_error_missing_close_brace) {
    char *err = NULL;
    AstNode *root = parse_source("chain MyChain { consensus: Tendermint", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "'}") != NULL);
    free(err);
}

TEST(test_error_missing_colon_in_assignment) {
    char *err = NULL;
    AstNode *root = parse_source("chain C { consensus Tendermint }", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "':' after subsystem name") != NULL);
    free(err);
}

TEST(test_error_unknown_subsystem_key) {
    /* `unknown:` lexes as IDENT, not one of the SUBSYSTEM keywords. */
    char *err = NULL;
    AstNode *root = parse_source("chain C { unknown: Tendermint }", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "expected a subsystem key") != NULL);
    free(err);
}

TEST(test_error_missing_type_expr_after_colon) {
    char *err = NULL;
    AstNode *root = parse_source("chain C { consensus: }", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "expected a type name") != NULL);
    free(err);
}

TEST(test_error_unterminated_generic_args) {
    char *err = NULL;
    AstNode *root = parse_source("chain C { consensus: Tendermint<sig=BLS12_381 }", &err);
    ASSERT(root == NULL);
    ASSERT(err && strstr(err, "'>") != NULL);
    free(err);
}

/* Source-span sanity: the chain decl's span starts at the `chain` keyword. */
TEST(test_chain_decl_span_starts_at_keyword) {
    const char *src = "  chain Tiny { exec: WasmVM }"; /* leading spaces shift cols */
    char *err = NULL;
    AstNode *root = parse_source(src, &err);
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->span.start_line, 1);
    ASSERT_EQ_INT(root->span.start_col, 3); /* col 3 = after "  " */
    free(err);
}

int main(void) {
    /* success path */
    RUN(test_parse_minimal_chain_block);
    RUN(test_parse_chain_with_generic_args_and_kwargs);
    RUN(test_parse_chain_with_set_literal_arg);
    RUN(test_parse_chain_with_all_four_subsystems);
    RUN(test_parse_empty_chain_block);

    /* error reporting */
    RUN(test_error_missing_chain_keyword);
    RUN(test_error_missing_chain_name);
    RUN(test_error_missing_open_brace);
    RUN(test_error_missing_close_brace);
    RUN(test_error_missing_colon_in_assignment);
    RUN(test_error_unknown_subsystem_key);
    RUN(test_error_missing_type_expr_after_colon);
    RUN(test_error_unterminated_generic_args);

    /* spans */
    RUN(test_chain_decl_span_starts_at_keyword);

    REPORT();
}
