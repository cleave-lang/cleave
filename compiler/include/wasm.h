#ifndef CLEAVE_WASM_H
#define CLEAVE_WASM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Low-level WebAssembly binary emitter (issue #17).
 *
 * Self-contained: no AST or type-checker dependency. The codegen layer
 * (compiler/src/codegen.c) builds Cleave-specific module structure on
 * top of these primitives.
 *
 * The WASM binary format is little-endian with variable-length integers
 * (LEB128) for indices and lengths. Sections carry a one-byte ID, a
 * length prefix, and contents. All entry points here append to a
 * growable byte buffer; the caller drives section ordering.
 *
 * The reference spec lives at
 *   https://webassembly.github.io/spec/core/binary/
 * but the subset Cleave uses today is small: type, import, function,
 * memory, export, code sections, plus a handful of opcodes documented
 * inline in codegen.c.
 */

/* Growable byte buffer. */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} WasmBuf;

/* ============== buffer lifecycle ============== */

void   wasm_init(WasmBuf *b);
void   wasm_free(WasmBuf *b);

/* Reset the buffer's length to zero without freeing the backing storage.
 * Useful when emitting a section body into a scratch buffer that gets
 * length-prefixed into the parent. */
void   wasm_reset(WasmBuf *b);

/* ============== raw writes ============== */

void   wasm_write_byte(WasmBuf *b, uint8_t v);
void   wasm_write_bytes(WasmBuf *b, const uint8_t *src, size_t n);
void   wasm_write_u32_le(WasmBuf *b, uint32_t v); /* fixed-width 4 bytes */

/* LEB128 integer encoding. Unsigned uses `u32`/`u64` semantics; signed
 * uses zig-zag-free signed-LEB128 per the WASM spec. */
void   wasm_write_leb_u32(WasmBuf *b, uint32_t v);
void   wasm_write_leb_u64(WasmBuf *b, uint64_t v);
void   wasm_write_leb_i32(WasmBuf *b, int32_t v);
void   wasm_write_leb_i64(WasmBuf *b, int64_t v);

/* WASM strings are LEB128-prefixed UTF-8. */
void   wasm_write_name(WasmBuf *b, const char *s, size_t length);

/* ============== sections ============== */

/* Section IDs per the WASM spec. */
enum {
    WASM_SECTION_CUSTOM   = 0,
    WASM_SECTION_TYPE     = 1,
    WASM_SECTION_IMPORT   = 2,
    WASM_SECTION_FUNCTION = 3,
    WASM_SECTION_TABLE    = 4,
    WASM_SECTION_MEMORY   = 5,
    WASM_SECTION_GLOBAL   = 6,
    WASM_SECTION_EXPORT   = 7,
    WASM_SECTION_START    = 8,
    WASM_SECTION_ELEMENT  = 9,
    WASM_SECTION_CODE     = 10,
    WASM_SECTION_DATA     = 11
};

/* Append section_id, LEB128 length of `body`, then `body` itself. */
void   wasm_emit_section(WasmBuf *out, uint8_t section_id,
                         const WasmBuf *body);

/* Magic + version: \0asm\1\0\0\0. The first thing in any .wasm file. */
void   wasm_emit_header(WasmBuf *b);

/* ============== value types + opcodes ============== */

/* WASM value-type byte encodings. */
enum {
    WASM_TYPE_I32      = 0x7F,
    WASM_TYPE_I64      = 0x7E,
    WASM_TYPE_F32      = 0x7D,
    WASM_TYPE_F64      = 0x7C,
    WASM_TYPE_FUNCREF  = 0x70,
    WASM_TYPE_EXTERNREF= 0x6F,
    WASM_TYPE_FUNC     = 0x60  /* used as the leading byte of a function type */
};

/* Opcodes used by Cleave codegen. Names mirror the spec's text format. */
enum {
    WASM_OP_BLOCK        = 0x02,
    WASM_OP_LOOP         = 0x03,
    WASM_OP_IF           = 0x04,
    WASM_OP_ELSE         = 0x05,
    WASM_OP_END          = 0x0B,
    WASM_OP_RETURN       = 0x0F,
    WASM_OP_CALL         = 0x10,
    WASM_OP_DROP         = 0x1A,
    WASM_OP_LOCAL_GET    = 0x20,
    WASM_OP_LOCAL_SET    = 0x21,
    WASM_OP_LOCAL_TEE    = 0x22,
    WASM_OP_I32_CONST    = 0x41,
    WASM_OP_I64_CONST    = 0x42,
    WASM_OP_I64_EQZ      = 0x50,
    WASM_OP_I64_EQ       = 0x51,
    WASM_OP_I64_NE       = 0x52,
    WASM_OP_I64_LT_S     = 0x53,
    WASM_OP_I64_LT_U     = 0x54,
    WASM_OP_I64_GT_S     = 0x55,
    WASM_OP_I64_GT_U     = 0x56,
    WASM_OP_I64_LE_S     = 0x57,
    WASM_OP_I64_LE_U     = 0x58,
    WASM_OP_I64_GE_S     = 0x59,
    WASM_OP_I64_GE_U     = 0x5A,
    WASM_OP_I64_ADD      = 0x7C,
    WASM_OP_I64_SUB      = 0x7D,
    WASM_OP_I64_MUL      = 0x7E,
    WASM_OP_I64_DIV_S    = 0x7F,
    WASM_OP_I64_DIV_U    = 0x80,
    WASM_OP_I64_REM_S    = 0x81,
    WASM_OP_I64_REM_U    = 0x82,
    WASM_OP_I64_AND      = 0x83,
    WASM_OP_I64_OR       = 0x84,
    WASM_OP_I32_WRAP_I64 = 0xA7   /* drops upper 32 bits of an i64; used to
                                     coerce a bool-shaped i64 (0/1) into an
                                     i32 for the if instruction */
};

/* Block-type byte that immediately follows a block / loop / if instruction.
 * `empty` means no result on the stack at the end of the block (the only
 * case the v0 codegen needs since if-as-expression is not yet supported). */
enum {
    WASM_BLOCKTYPE_EMPTY = 0x40
};

/* Export descriptor kinds. */
enum {
    WASM_EXPORT_FUNC   = 0x00,
    WASM_EXPORT_TABLE  = 0x01,
    WASM_EXPORT_MEMORY = 0x02,
    WASM_EXPORT_GLOBAL = 0x03
};

#endif /* CLEAVE_WASM_H */
