#ifndef CLEAVE_CODEGEN_H
#define CLEAVE_CODEGEN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ast.h"
#include "wasm.h"

/*
 * Cleave -> WebAssembly code generator (issue #17).
 *
 * Input: a typed AST produced by the parser + type checker.
 * Output: a complete WASM 1.0 binary that the runtime can AOT-compile
 *         via Wasmtime.
 *
 * What this layer currently supports:
 *
 *   - Module declarations with `state` and `fn` items.
 *   - Function bodies: integer literals, identifier reads (state slots,
 *     locals not yet), binary +, -, *, /, equality/ordering ops, function
 *     calls between module fns, assignment to state, block trailing
 *     expressions used as function return values.
 *
 * What it does NOT yet do (each is its own future issue):
 *
 *   - `let` bindings (need local-slot allocation)
 *   - Control flow beyond straight-line: `if`/`else`, `match`, loops
 *   - Sum / option types (no Result yet; `Ok(x)` is unsupported)
 *   - Events, gas-consume calls, deferred effects
 *   - Multiple modules per file
 *   - Cross-module calls
 *
 * Hostcall ABI is specified in spec/abi/wasm.md.
 */

typedef enum {
    CG_OK = 0,
    CG_ERR = 1
} CodegenStatus;

typedef struct {
    FILE *err;     /* where diagnostics go (defaults to stderr) */
    int  n_errors; /* count of reported errors */
} Codegen;

/* Initialize a code generator with a diagnostic sink. */
void cg_init(Codegen *cg, FILE *err);

/* Generate WASM bytes for the given program (the synthetic module
 * returned by parser_parse_program). The first module-declaration found
 * inside is compiled; chains and protocols are ignored at this layer.
 * On success returns CG_OK and writes the binary into *out (caller owns
 * the buffer and must wasm_free it). On error returns CG_ERR and leaves
 * the buffer untouched. */
CodegenStatus cg_compile_program(Codegen *cg, AstNode *program, WasmBuf *out);

#endif /* CLEAVE_CODEGEN_H */
