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

/* Parse a source string as a full program (one or more top-level decls). */
static AstNode *parse_program(const char *src, char **err_buf) {
    size_t err_size = 0;
    FILE *captured = stderr_to_buf(err_buf, &err_size);
    FILE *saved_stderr = stderr;
    if (captured) stderr = captured;

    Lexer lex;
    lexer_init(&lex, src);

    Parser p;
    parser_init(&p, &lex);

    AstNode *root = parser_parse_program(&p);

    if (captured) {
        fflush(captured);
        fclose(captured);
        stderr = saved_stderr;
    }

    if (p.had_error) return NULL;
    return root;
}

/* Parse a source string as a bare expression. */
static AstNode *parse_expr(const char *src, char **err_buf) {
    size_t err_size = 0;
    FILE *captured = stderr_to_buf(err_buf, &err_size);
    FILE *saved_stderr = stderr;
    if (captured) stderr = captured;

    Lexer lex;
    lexer_init(&lex, src);

    Parser p;
    parser_init(&p, &lex);

    AstNode *root = parser_parse_expression(&p);

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

/* ============== expression tests (issue #35) ============== */

TEST(test_expr_number) {
    char *err = NULL;
    AstNode *e = parse_expr("42", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_NUMBER);
    ASSERT_EQ_STR_LEN(e->as.number.text.start, e->as.number.text.length, "42");
    free(err);
}

TEST(test_expr_binary_precedence) {
    /* 1 + 2 * 3 must parse as 1 + (2 * 3). */
    char *err = NULL;
    AstNode *e = parse_expr("1 + 2 * 3", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_BINARY);
    ASSERT_EQ_STR_LEN(e->as.binary.op.start, e->as.binary.op.length, "+");
    ASSERT_EQ_INT(e->as.binary.lhs->kind, AST_EXPR_NUMBER);
    ASSERT_EQ_INT(e->as.binary.rhs->kind, AST_EXPR_BINARY);
    ASSERT_EQ_STR_LEN(e->as.binary.rhs->as.binary.op.start,
                      e->as.binary.rhs->as.binary.op.length, "*");
    free(err);
}

TEST(test_expr_left_associativity) {
    /* a - b - c must parse as (a - b) - c. */
    char *err = NULL;
    AstNode *e = parse_expr("a - b - c", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_BINARY);
    ASSERT_EQ_STR_LEN(e->as.binary.op.start, e->as.binary.op.length, "-");
    ASSERT_EQ_INT(e->as.binary.lhs->kind, AST_EXPR_BINARY);
    ASSERT_EQ_INT(e->as.binary.rhs->kind, AST_EXPR_IDENT);
    free(err);
}

TEST(test_expr_unary_then_postfix) {
    /* -f(x).y */
    char *err = NULL;
    AstNode *e = parse_expr("-f(x).y", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_UNARY);
    ASSERT_EQ_STR_LEN(e->as.unary.op.start, e->as.unary.op.length, "-");
    ASSERT_EQ_INT(e->as.unary.operand->kind, AST_EXPR_BINARY);
    ASSERT_EQ_STR_LEN(e->as.unary.operand->as.binary.op.start,
                      e->as.unary.operand->as.binary.op.length, ".");
    free(err);
}

TEST(test_expr_call_with_args) {
    char *err = NULL;
    AstNode *e = parse_expr("f(1, 2)", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_CALL);
    ASSERT_EQ_INT(e->as.call.n_args, 2);
    free(err);
}

TEST(test_expr_index) {
    char *err = NULL;
    AstNode *e = parse_expr("a[b + 1]", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_INDEX);
    ASSERT_EQ_INT(e->as.index.index->kind, AST_EXPR_BINARY);
    free(err);
}

TEST(test_expr_path) {
    char *err = NULL;
    AstNode *e = parse_expr("Foo::bar", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_BINARY);
    ASSERT_EQ_STR_LEN(e->as.binary.op.start, e->as.binary.op.length, "::");
    free(err);
}

TEST(test_expr_parenthesized) {
    /* (a + b) * c must group correctly. */
    char *err = NULL;
    AstNode *e = parse_expr("(a + b) * c", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_BINARY);
    ASSERT_EQ_STR_LEN(e->as.binary.op.start, e->as.binary.op.length, "*");
    ASSERT_EQ_INT(e->as.binary.lhs->kind, AST_EXPR_BINARY);
    ASSERT_EQ_STR_LEN(e->as.binary.lhs->as.binary.op.start,
                      e->as.binary.lhs->as.binary.op.length, "+");
    free(err);
}

TEST(test_expr_bool_and_null) {
    char *err = NULL;
    AstNode *t = parse_expr("true", &err);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->kind, AST_EXPR_BOOL);
    ASSERT_EQ_INT(t->as.boolean.value, 1);

    AstNode *n = parse_expr("null", &err);
    ASSERT(n != NULL);
    ASSERT_EQ_INT(n->kind, AST_EXPR_NULL);
    free(err);
}

TEST(test_expr_record_literal) {
    char *err = NULL;
    AstNode *e = parse_expr("{ cpu: 100, storage: 8 }", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_RECORD);
    ASSERT_EQ_INT(e->as.record.n_fields, 2);
    AstNode *f0 = e->as.record.fields[0];
    ASSERT_EQ_INT(f0->kind, AST_RECORD_FIELD);
    ASSERT_EQ_STR_LEN(f0->as.record_field.key.start,
                      f0->as.record_field.key.length, "cpu");
    free(err);
}

TEST(test_expr_block_with_trailing_expression) {
    char *err = NULL;
    AstNode *e = parse_expr("{ let x = 1; x + 2 }", &err);
    ASSERT(e != NULL);
    ASSERT_EQ_INT(e->kind, AST_EXPR_BLOCK);
    ASSERT_EQ_INT(e->as.block.n_stmts, 1);
    ASSERT(e->as.block.result != NULL);
    ASSERT_EQ_INT(e->as.block.result->kind, AST_EXPR_BINARY);
    free(err);
}

/* ============== statement + declaration tests ============== */

TEST(test_module_with_state_and_fn) {
    const char *src =
        "module Counter {\n"
        "    state count: u64\n"
        "    fn read() -> u64 { count }\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    /* root is a synthetic program-module wrapping the real Counter module */
    ASSERT_EQ_INT(root->kind, AST_MODULE_DECL);
    ASSERT_EQ_INT(root->as.module.n_items, 1);
    AstNode *m = root->as.module.items[0];
    ASSERT_EQ_INT(m->kind, AST_MODULE_DECL);
    ASSERT_EQ_STR_LEN(m->as.module.name.start, m->as.module.name.length, "Counter");
    ASSERT_EQ_INT(m->as.module.n_items, 2);
    ASSERT_EQ_INT(m->as.module.items[0]->kind, AST_STATE_DECL);
    ASSERT_EQ_INT(m->as.module.items[1]->kind, AST_FN_DECL);
    free(err);
}

TEST(test_fn_with_pure_modifier_and_with_effects) {
    const char *src =
        "module M {\n"
        "  pure fn f(x: u64) -> u64 with [Log, Emit] { x }\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *m = root->as.module.items[0];
    AstNode *fn = m->as.module.items[0];
    ASSERT_EQ_INT(fn->kind, AST_FN_DECL);
    ASSERT_EQ_INT(fn->as.fn.modifier, FN_MOD_PURE);
    ASSERT_EQ_INT(fn->as.fn.n_with_effects, 2);
    ASSERT_EQ_STR_LEN(fn->as.fn.with_effects[0].start,
                      fn->as.fn.with_effects[0].length, "Log");
    ASSERT_EQ_STR_LEN(fn->as.fn.with_effects[1].start,
                      fn->as.fn.with_effects[1].length, "Emit");
    free(err);
}

TEST(test_event_and_effect_declarations) {
    const char *src =
        "module M {\n"
        "  event Transferred(amount: u64)\n"
        "  effect publish(msg: Bytes) -> Hash deferred\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *m = root->as.module.items[0];
    ASSERT_EQ_INT(m->as.module.n_items, 2);
    AstNode *ev = m->as.module.items[0];
    ASSERT_EQ_INT(ev->kind, AST_EVENT_DECL);
    ASSERT_EQ_INT(ev->as.event.n_params, 1);
    AstNode *ef = m->as.module.items[1];
    ASSERT_EQ_INT(ef->kind, AST_EFFECT_DECL);
    ASSERT_EQ_INT(ef->as.effect.is_deferred, 1);
    ASSERT(ef->as.effect.return_type != NULL);
    free(err);
}

TEST(test_gas_declaration_with_record_literal) {
    const char *src =
        "module M {\n"
        "  gas g = { cpu: 100, storage: 8 }\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *m = root->as.module.items[0];
    AstNode *g = m->as.module.items[0];
    ASSERT_EQ_INT(g->kind, AST_GAS_DECL);
    ASSERT_EQ_INT(g->as.gas.value->kind, AST_EXPR_RECORD);
    ASSERT_EQ_INT(g->as.gas.value->as.record.n_fields, 2);
    free(err);
}

TEST(test_let_with_type_annotation) {
    /* let inside a fn body */
    const char *src =
        "module M {\n"
        "  fn f() { let x: u64 = 1 }\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *fn = root->as.module.items[0]->as.module.items[0];
    AstNode *body = fn->as.fn.body;
    ASSERT_EQ_INT(body->kind, AST_EXPR_BLOCK);
    ASSERT_EQ_INT(body->as.block.n_stmts, 1);
    AstNode *let = body->as.block.stmts[0];
    ASSERT_EQ_INT(let->kind, AST_STMT_LET);
    ASSERT(let->as.let_stmt.type != NULL);
    free(err);
}

TEST(test_if_else_chain) {
    const char *src =
        "module M {\n"
        "  fn f() { if a { 1 } else if b { 2 } else { 3 } }\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *fn = root->as.module.items[0]->as.module.items[0];
    AstNode *body = fn->as.fn.body;
    AstNode *if1 = body->as.block.stmts[0];
    ASSERT_EQ_INT(if1->kind, AST_STMT_IF);
    AstNode *if2 = if1->as.if_stmt.else_branch;
    ASSERT(if2 != NULL);
    ASSERT_EQ_INT(if2->kind, AST_STMT_IF);
    ASSERT(if2->as.if_stmt.else_branch != NULL);
    ASSERT_EQ_INT(if2->as.if_stmt.else_branch->kind, AST_EXPR_BLOCK);
    free(err);
}

TEST(test_match_with_arms) {
    const char *src =
        "module M {\n"
        "  fn f() { match x { 1 => a, 2 => b } }\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *fn = root->as.module.items[0]->as.module.items[0];
    AstNode *m = fn->as.fn.body->as.block.stmts[0];
    ASSERT_EQ_INT(m->kind, AST_STMT_MATCH);
    ASSERT_EQ_INT(m->as.match_stmt.n_arms, 2);
    free(err);
}

TEST(test_return_with_value) {
    const char *src =
        "module M {\n"
        "  fn f() -> u64 { return 42 }\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *fn = root->as.module.items[0]->as.module.items[0];
    AstNode *ret = fn->as.fn.body->as.block.stmts[0];
    ASSERT_EQ_INT(ret->kind, AST_STMT_RETURN);
    ASSERT(ret->as.ret_stmt.value != NULL);
    ASSERT_EQ_INT(ret->as.ret_stmt.value->kind, AST_EXPR_NUMBER);
    free(err);
}

TEST(test_protocol_with_implements_and_slash_on) {
    const char *src =
        "protocol P implements ConsensusProtocol {\n"
        "  effect propose() -> Block\n"
        "  slash_on Evidence { who: Address }\n"
        "    when true\n"
        "    penalty 1\n"
        "}";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    AstNode *proto = root->as.module.items[0];
    ASSERT_EQ_INT(proto->kind, AST_PROTOCOL_DECL);
    ASSERT(proto->as.protocol.implements != NULL);
    ASSERT_EQ_INT(proto->as.protocol.n_items, 2);
    AstNode *so = proto->as.protocol.items[1];
    ASSERT_EQ_INT(so->kind, AST_SLASH_ON_DECL);
    ASSERT(so->as.slash_on.when_clause != NULL);
    ASSERT(so->as.slash_on.penalty_clause != NULL);
    free(err);
}

TEST(test_program_with_multiple_top_level_decls) {
    const char *src =
        "chain C { consensus: Aura }\n"
        "module M { state x: u64 }\n";
    char *err = NULL;
    AstNode *root = parse_program(src, &err);
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->as.module.n_items, 2);
    ASSERT_EQ_INT(root->as.module.items[0]->kind, AST_CHAIN_DECL);
    ASSERT_EQ_INT(root->as.module.items[1]->kind, AST_MODULE_DECL);
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

    /* expressions */
    RUN(test_expr_number);
    RUN(test_expr_binary_precedence);
    RUN(test_expr_left_associativity);
    RUN(test_expr_unary_then_postfix);
    RUN(test_expr_call_with_args);
    RUN(test_expr_index);
    RUN(test_expr_path);
    RUN(test_expr_parenthesized);
    RUN(test_expr_bool_and_null);
    RUN(test_expr_record_literal);
    RUN(test_expr_block_with_trailing_expression);

    /* statements + declarations */
    RUN(test_module_with_state_and_fn);
    RUN(test_fn_with_pure_modifier_and_with_effects);
    RUN(test_event_and_effect_declarations);
    RUN(test_gas_declaration_with_record_literal);
    RUN(test_let_with_type_annotation);
    RUN(test_if_else_chain);
    RUN(test_match_with_arms);
    RUN(test_return_with_value);
    RUN(test_protocol_with_implements_and_slash_on);
    RUN(test_program_with_multiple_top_level_decls);

    REPORT();
}
