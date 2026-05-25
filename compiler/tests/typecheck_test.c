#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "typecheck.h"

/* ============== helpers ============== */

/* Run lex + parse + typecheck on a source string. Sets *out_errors to the
 * count of type errors reported (0 on success). All diagnostics from both
 * the parser and the type checker are discarded into a buffer that the
 * caller frees. Returns 1 if parsing succeeded, 0 if parsing failed. */
static int run_check(const char *src, int *out_errors, char **err_buf) {
    size_t err_size = 0;
    FILE *captured = open_memstream(err_buf, &err_size);
    if (!captured) return 0;

    Lexer lex;
    lexer_init(&lex, src);

    Parser parser;
    parser_init(&parser, &lex);

    /* Route parser errors into the same capture so tests stay quiet. */
    FILE *saved_stderr = stderr;
    stderr = captured;
    AstNode *program = parser_parse_program(&parser);
    stderr = saved_stderr;

    if (!program || parser_had_error(&parser)) {
        fclose(captured);
        return 0;
    }

    TypeChecker tc;
    typecheck_init(&tc, captured);
    (void)typecheck_program(&tc, program);
    fclose(captured);

    *out_errors = tc.n_errors;
    return 1;
}

/* ============== type singletons ============== */

TEST(test_prim_interning) {
    const Type *a = type_prim(PRIM_U64);
    const Type *b = type_prim(PRIM_U64);
    ASSERT(a == b);

    const Type *c = type_prim(PRIM_BOOL);
    ASSERT(c != a);
}

TEST(test_describe_primitives) {
    ASSERT(strcmp(type_describe(type_prim(PRIM_U64)), "u64") == 0);
    ASSERT(strcmp(type_describe(type_prim(PRIM_BOOL)), "bool") == 0);
    ASSERT(strcmp(type_describe(type_unit()), "()") == 0);
    ASSERT(strcmp(type_describe(type_unknown()), "<unknown>") == 0);
}

/* ============== positive cases ============== */

TEST(test_counter_module_typechecks) {
    const char *src =
        "module Counter {\n"
        "  state count: u64\n"
        "  fn increment() -> u64 { count = count + 1; count }\n"
        "  fn read() -> u64 { count }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    int parsed = run_check(src, &errors, &err);
    ASSERT(parsed);
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

TEST(test_let_without_annotation_infers_type) {
    const char *src =
        "module M {\n"
        "  fn f() -> u64 { let x = 1; x }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

TEST(test_comparison_produces_bool) {
    const char *src =
        "module M {\n"
        "  fn f() -> bool { 1 == 1 }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

TEST(test_logical_op_requires_bool_operands) {
    const char *src =
        "module M {\n"
        "  fn f() -> bool { true && false }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

TEST(test_chain_manifest_typechecks) {
    const char *src =
        "chain C {\n"
        "  consensus: Tendermint<sig=BLS12_381>\n"
        "  gas: Multidim<{cpu, storage}>\n"
        "  state: SparseMerkle<key=u256, hash=Blake3>\n"
        "  exec: WasmVM<deterministic>\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

/* ============== negative cases ============== */

TEST(test_let_initializer_type_mismatch) {
    const char *src =
        "module M {\n"
        "  fn f() { let x: bool = 1 }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_assignment_type_mismatch) {
    const char *src =
        "module M {\n"
        "  state count: u64\n"
        "  fn f() { count = true }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_return_type_mismatch) {
    const char *src =
        "module M {\n"
        "  fn f() -> u64 { true }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_logical_op_rejects_non_bool) {
    const char *src =
        "module M {\n"
        "  fn f() -> bool { 1 && true }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_if_condition_must_be_bool) {
    const char *src =
        "module M {\n"
        "  fn f() { if 1 { let x = 1 } }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_comparison_with_mismatched_operands) {
    const char *src =
        "module M {\n"
        "  fn f() -> bool { true == 1 }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_call_arity_mismatch) {
    const char *src =
        "module M {\n"
        "  fn add(a: u64, b: u64) -> u64 { a + b }\n"
        "  fn use() -> u64 { add(1) }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_call_arg_type_mismatch) {
    const char *src =
        "module M {\n"
        "  fn want_bool(b: bool) -> u64 { 0 }\n"
        "  fn use() -> u64 { want_bool(1) }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    free(err);
}

TEST(test_continues_after_first_error) {
    const char *src =
        "module M {\n"
        "  fn f() { let x: bool = 1; let y: bool = 2 }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 2);
    free(err);
}

/* ============== move tracking (RFC 0001 Phase 1) ============== */

TEST(test_copy_type_can_be_used_repeatedly) {
    /* Primitives are Copy; using `x` multiple times is fine. */
    const char *src =
        "module M {\n"
        "  fn f() -> u64 { let x: u64 = 42; let y: u64 = x; let z: u64 = x; z }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

TEST(test_non_copy_use_after_move_in_let_errors) {
    /* `make_result` returns Result<u64> which is TY_GENERIC -> non-Copy.
     * `let y = r` moves r; the subsequent `let z = r` reads a moved
     * value and should error. */
    const char *src =
        "module M {\n"
        "  fn make() -> Result<u64> { make() }\n"
        "  fn use_twice() -> Result<u64> {\n"
        "    let r: Result<u64> = make()\n"
        "    let y: Result<u64> = r\n"
        "    let z: Result<u64> = r\n"
        "    z\n"
        "  }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    ASSERT(err && strstr(err, "use of moved value 'r'") != NULL);
    free(err);
}

TEST(test_non_copy_use_after_move_via_call_errors) {
    /* Passing r to take(r) moves it; subsequent reference errors. */
    const char *src =
        "module M {\n"
        "  fn make() -> Result<u64> { make() }\n"
        "  fn take(r: Result<u64>) -> u64 { 0 }\n"
        "  fn use_then_call() -> Result<u64> {\n"
        "    let r: Result<u64> = make()\n"
        "    let _: u64 = take(r)\n"
        "    r\n"
        "  }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT(errors >= 1);
    ASSERT(err && strstr(err, "use of moved value 'r'") != NULL);
    free(err);
}

TEST(test_non_copy_single_use_is_fine) {
    /* Moving once is the happy path; only the second use should error. */
    const char *src =
        "module M {\n"
        "  fn make() -> Result<u64> { make() }\n"
        "  fn ok() -> Result<u64> {\n"
        "    let r: Result<u64> = make()\n"
        "    r\n"
        "  }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

TEST(test_copy_after_move_irrelevant) {
    /* `x: u64` is Copy. Both `let y = x` and the later read of `x` are
     * value copies; nothing moves. No diagnostic should fire even
     * though syntactically this looks like a use-after-move pattern. */
    const char *src =
        "module M {\n"
        "  fn read(x: u64) -> u64 { x }\n"
        "  fn caller() -> u64 {\n"
        "    let x: u64 = 5\n"
        "    let y: u64 = x\n"
        "    let z: u64 = x\n"
        "    read(x)\n"
        "  }\n"
        "}";
    int errors = 0;
    char *err = NULL;
    ASSERT(run_check(src, &errors, &err));
    ASSERT_EQ_INT(errors, 0);
    free(err);
}

TEST(test_type_is_copy_classification) {
    /* Direct API test of the Copy classification. */
    ASSERT_EQ_INT(type_is_copy(type_prim(PRIM_U64)), 1);
    ASSERT_EQ_INT(type_is_copy(type_prim(PRIM_BOOL)), 1);
    ASSERT_EQ_INT(type_is_copy(type_prim(PRIM_STR)), 1);
    ASSERT_EQ_INT(type_is_copy(type_unit()), 1);
    ASSERT_EQ_INT(type_is_copy(type_unknown()), 1);
    ASSERT_EQ_INT(type_is_copy(NULL), 1);
    /* No public API for constructing a generic type; covered by the
     * end-to-end use-after-move tests above. */
}

int main(void) {
    /* type singletons */
    RUN(test_prim_interning);
    RUN(test_describe_primitives);

    /* positive */
    RUN(test_counter_module_typechecks);
    RUN(test_let_without_annotation_infers_type);
    RUN(test_comparison_produces_bool);
    RUN(test_logical_op_requires_bool_operands);
    RUN(test_chain_manifest_typechecks);

    /* negative */
    RUN(test_let_initializer_type_mismatch);
    RUN(test_assignment_type_mismatch);
    RUN(test_return_type_mismatch);
    RUN(test_logical_op_rejects_non_bool);
    RUN(test_if_condition_must_be_bool);
    RUN(test_comparison_with_mismatched_operands);
    RUN(test_call_arity_mismatch);
    RUN(test_call_arg_type_mismatch);
    RUN(test_continues_after_first_error);

    /* move tracking (RFC 0001 Phase 1) */
    RUN(test_copy_type_can_be_used_repeatedly);
    RUN(test_non_copy_use_after_move_in_let_errors);
    RUN(test_non_copy_use_after_move_via_call_errors);
    RUN(test_non_copy_single_use_is_fine);
    RUN(test_copy_after_move_irrelevant);
    RUN(test_type_is_copy_classification);

    REPORT();
}
