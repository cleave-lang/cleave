#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "typecheck.h"
#include "wasm.h"

/* ============== helpers ============== */

/* Lex + parse + typecheck + codegen the given source. Returns 1 on
 * success and fills *out with the WASM bytes (caller wasm_free's). */
static int compile_source(const char *src, WasmBuf *out) {
    Lexer lex;
    lexer_init(&lex, src);

    Parser parser;
    parser_init(&parser, &lex);

    /* Route parser + typecheck + codegen diagnostics to a discard sink
     * so tests stay quiet. */
    FILE *devnull = fopen("/dev/null", "w");
    if (!devnull) return 0;

    FILE *saved_stderr = stderr;
    stderr = devnull;
    AstNode *program = parser_parse_program(&parser);
    stderr = saved_stderr;

    if (!program || parser_had_error(&parser)) { fclose(devnull); return 0; }

    TypeChecker tc;
    typecheck_init(&tc, devnull);
    if (typecheck_program(&tc, program) != 0) { fclose(devnull); return 0; }

    Codegen cg;
    cg_init(&cg, devnull);
    CodegenStatus rc = cg_compile_program(&cg, program, out);
    fclose(devnull);
    return rc == CG_OK;
}

static int starts_with_bytes(const WasmBuf *b, const uint8_t *prefix, size_t n) {
    if (b->len < n) return 0;
    return memcmp(b->data, prefix, n) == 0;
}

static int contains_bytes(const WasmBuf *b, const uint8_t *needle, size_t n) {
    if (b->len < n) return 0;
    for (size_t i = 0; i + n <= b->len; ++i) {
        if (memcmp(b->data + i, needle, n) == 0) return 1;
    }
    return 0;
}

/* ============== LEB128 unit tests ============== */

TEST(test_leb_u32_zero) {
    WasmBuf b; wasm_init(&b);
    wasm_write_leb_u32(&b, 0);
    ASSERT_EQ_INT(b.len, 1);
    ASSERT_EQ_INT(b.data[0], 0);
    wasm_free(&b);
}

TEST(test_leb_u32_127) {
    WasmBuf b; wasm_init(&b);
    wasm_write_leb_u32(&b, 127);
    ASSERT_EQ_INT(b.len, 1);
    ASSERT_EQ_INT(b.data[0], 0x7F);
    wasm_free(&b);
}

TEST(test_leb_u32_128) {
    WasmBuf b; wasm_init(&b);
    wasm_write_leb_u32(&b, 128);
    ASSERT_EQ_INT(b.len, 2);
    ASSERT_EQ_INT(b.data[0], 0x80);
    ASSERT_EQ_INT(b.data[1], 0x01);
    wasm_free(&b);
}

TEST(test_leb_i64_signed_negative) {
    WasmBuf b; wasm_init(&b);
    wasm_write_leb_i64(&b, -1);
    ASSERT_EQ_INT(b.len, 1);
    ASSERT_EQ_INT(b.data[0], 0x7F);
    wasm_free(&b);
}

TEST(test_leb_i64_signed_positive) {
    WasmBuf b; wasm_init(&b);
    wasm_write_leb_i64(&b, 1);
    ASSERT_EQ_INT(b.len, 1);
    ASSERT_EQ_INT(b.data[0], 0x01);
    wasm_free(&b);
}

TEST(test_header_is_magic_and_version) {
    WasmBuf b; wasm_init(&b);
    wasm_emit_header(&b);
    ASSERT_EQ_INT(b.len, 8);
    uint8_t expected[] = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
    ASSERT(memcmp(b.data, expected, 8) == 0);
    wasm_free(&b);
}

/* ============== full module byte-level checks ============== */

TEST(test_counter_module_compiles_to_valid_header) {
    const char *src =
        "module Counter {\n"
        "  state count: u64\n"
        "  fn read() -> u64 { count }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));

    /* Magic + version come first. */
    uint8_t header[] = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
    ASSERT(starts_with_bytes(&bin, header, 8));
    wasm_free(&bin);
}

TEST(test_counter_module_imports_env_state_get) {
    const char *src =
        "module Counter {\n"
        "  state count: u64\n"
        "  fn read() -> u64 { count }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    /* `env` followed by `state_get` as length-prefixed names. */
    const uint8_t needle[] = {
        0x03, 'e','n','v',
        0x09, 's','t','a','t','e','_','g','e','t'
    };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_counter_module_imports_env_state_set) {
    const char *src =
        "module Counter {\n"
        "  state count: u64\n"
        "  fn write() -> u64 { count = 1; 0 }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    const uint8_t needle[] = {
        0x03, 'e','n','v',
        0x09, 's','t','a','t','e','_','s','e','t'
    };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_module_fn_is_exported_under_source_name) {
    const char *src =
        "module M {\n"
        "  fn my_fn() -> u64 { 42 }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    /* Export name is `my_fn` length-prefixed. */
    const uint8_t needle[] = {
        0x05, 'm','y','_','f','n'
    };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_integer_literal_emits_i64_const) {
    const char *src =
        "module M {\n"
        "  fn f() -> u64 { 42 }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    /* i64.const opcode followed by 42's LEB128 encoding (single byte 0x2A). */
    const uint8_t needle[] = { 0x42, 0x2A };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_addition_emits_i64_add_opcode) {
    const char *src =
        "module M {\n"
        "  fn f() -> u64 { 1 + 2 }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    /* i64.const 1 (0x42 0x01), i64.const 2 (0x42 0x02), i64.add (0x7C). */
    const uint8_t needle[] = { 0x42, 0x01, 0x42, 0x02, 0x7C };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_assignment_emits_state_set_call) {
    const char *src =
        "module M {\n"
        "  state x: u64\n"
        "  fn f() -> u64 { x = 7; 0 }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    /* i32.const 0 (slot 0), i64.const 7, call (1, the state_set import). */
    const uint8_t needle[] = { 0x41, 0x00, 0x42, 0x07, 0x10, 0x01 };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_state_read_emits_state_get_call) {
    const char *src =
        "module M {\n"
        "  state x: u64\n"
        "  fn f() -> u64 { x }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    /* i32.const 0 (slot 0), call 0 (state_get import). */
    const uint8_t needle[] = { 0x41, 0x00, 0x10, 0x00 };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_function_call_emits_call_to_local_fn_index) {
    const char *src =
        "module M {\n"
        "  fn helper() -> u64 { 1 }\n"
        "  fn caller() -> u64 { helper() }\n"
        "}";
    WasmBuf bin;
    ASSERT(compile_source(src, &bin));
    /* helper is fn index 2; caller body calls it via call 2 (0x10 0x02). */
    const uint8_t needle[] = { 0x10, 0x02 };
    ASSERT(contains_bytes(&bin, needle, sizeof(needle)));
    wasm_free(&bin);
}

TEST(test_chain_only_program_is_rejected) {
    /* Chains have no executable functions; codegen should fail with a
     * "no module declaration" diagnostic. */
    const char *src = "chain C { consensus: Aura }";
    WasmBuf bin;
    int ok = compile_source(src, &bin);
    ASSERT(!ok);
    /* compile_source freed nothing in the failure path; bin was never
     * initialized by codegen, so do not free it. */
}

int main(void) {
    /* LEB128 primitives */
    RUN(test_leb_u32_zero);
    RUN(test_leb_u32_127);
    RUN(test_leb_u32_128);
    RUN(test_leb_i64_signed_negative);
    RUN(test_leb_i64_signed_positive);
    RUN(test_header_is_magic_and_version);

    /* module-shape byte-level checks */
    RUN(test_counter_module_compiles_to_valid_header);
    RUN(test_counter_module_imports_env_state_get);
    RUN(test_counter_module_imports_env_state_set);
    RUN(test_module_fn_is_exported_under_source_name);
    RUN(test_integer_literal_emits_i64_const);
    RUN(test_addition_emits_i64_add_opcode);
    RUN(test_assignment_emits_state_set_call);
    RUN(test_state_read_emits_state_get_call);
    RUN(test_function_call_emits_call_to_local_fn_index);
    RUN(test_chain_only_program_is_rejected);

    REPORT();
}
