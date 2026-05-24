/*
 * WebAssembly binary emitter primitives.
 *
 * All output is little-endian, with LEB128 variable-length integers for
 * indices and lengths. See compiler/include/wasm.h and the reference
 * spec at https://webassembly.github.io/spec/core/binary/.
 *
 * Memory: WasmBuf grows on demand with doubling. Callers free with
 * wasm_free. Codegen produces one final buffer per module; intermediate
 * scratch buffers are reused via wasm_reset to keep allocator pressure
 * low.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm.h"

static void grow(WasmBuf *b, size_t need) {
    if (b->cap >= need) return;
    size_t cap = b->cap == 0 ? 64 : b->cap;
    while (cap < need) cap *= 2;
    uint8_t *next = realloc(b->data, cap);
    if (!next) {
        fputs("fatal: out of memory in wasm emitter\n", stderr);
        exit(1);
    }
    b->data = next;
    b->cap = cap;
}

void wasm_init(WasmBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void wasm_free(WasmBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void wasm_reset(WasmBuf *b) {
    b->len = 0;
}

void wasm_write_byte(WasmBuf *b, uint8_t v) {
    grow(b, b->len + 1);
    b->data[b->len++] = v;
}

void wasm_write_bytes(WasmBuf *b, const uint8_t *src, size_t n) {
    grow(b, b->len + n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

void wasm_write_u32_le(WasmBuf *b, uint32_t v) {
    wasm_write_byte(b, (uint8_t)(v & 0xFF));
    wasm_write_byte(b, (uint8_t)((v >> 8) & 0xFF));
    wasm_write_byte(b, (uint8_t)((v >> 16) & 0xFF));
    wasm_write_byte(b, (uint8_t)((v >> 24) & 0xFF));
}

void wasm_write_leb_u32(WasmBuf *b, uint32_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v != 0) byte |= 0x80;
        wasm_write_byte(b, byte);
    } while (v != 0);
}

void wasm_write_leb_u64(WasmBuf *b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v != 0) byte |= 0x80;
        wasm_write_byte(b, byte);
    } while (v != 0);
}

void wasm_write_leb_i32(WasmBuf *b, int32_t v) {
    int more = 1;
    while (more) {
        uint8_t byte = (uint8_t)(v & 0x7F);
        /* arithmetic shift to preserve sign on the high bits */
        v >>= 7;
        int sign_bit = byte & 0x40;
        if ((v == 0 && !sign_bit) || (v == -1 && sign_bit)) {
            more = 0;
        } else {
            byte |= 0x80;
        }
        wasm_write_byte(b, byte);
    }
}

void wasm_write_leb_i64(WasmBuf *b, int64_t v) {
    int more = 1;
    while (more) {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        int sign_bit = byte & 0x40;
        if ((v == 0 && !sign_bit) || (v == -1 && sign_bit)) {
            more = 0;
        } else {
            byte |= 0x80;
        }
        wasm_write_byte(b, byte);
    }
}

void wasm_write_name(WasmBuf *b, const char *s, size_t length) {
    wasm_write_leb_u32(b, (uint32_t)length);
    wasm_write_bytes(b, (const uint8_t *)s, length);
}

void wasm_emit_section(WasmBuf *out, uint8_t section_id, const WasmBuf *body) {
    wasm_write_byte(out, section_id);
    wasm_write_leb_u32(out, (uint32_t)body->len);
    wasm_write_bytes(out, body->data, body->len);
}

void wasm_emit_header(WasmBuf *b) {
    /* \0asm magic */
    wasm_write_byte(b, 0x00);
    wasm_write_byte(b, 0x61);
    wasm_write_byte(b, 0x73);
    wasm_write_byte(b, 0x6D);
    /* version 1, little-endian */
    wasm_write_u32_le(b, 0x00000001);
}
