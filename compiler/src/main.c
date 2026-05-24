#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "cleave.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "typecheck.h"
#include "wasm.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "usage:\n"
            "  %s --version\n"
            "  %s --help\n"
            "  %s --tokens <file.cv>\n"
            "  %s --ast <file.cv>\n"
            "  %s --check <file.cv>\n"
            "  %s --emit-wasm <file.cv> [-o <out.wasm>]\n",
            prog, prog, prog, prog, prog, prog);
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        fprintf(stderr, "error: cannot seek '%s'\n", path);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        fprintf(stderr, "error: cannot tell size of '%s'\n", path);
        return NULL;
    }
    rewind(fp);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        fprintf(stderr, "error: out of memory reading '%s'\n", path);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, fp);
    fclose(fp);

    if (read != (size_t)size) {
        free(buf);
        fprintf(stderr, "error: short read on '%s'\n", path);
        return NULL;
    }

    buf[size] = '\0';
    return buf;
}

static int cmd_tokens(const char *path) {
    char *source = read_file(path);
    if (!source) return 1;

    Lexer lex;
    lexer_init(&lex, source);

    for (;;) {
        Token t = lexer_next(&lex);
        printf("%4zu:%-3zu  %-14s  '%.*s'\n",
               t.line, t.column,
               token_kind_name(t.kind),
               (int)t.length, t.start);
        if (t.kind == TK_EOF) break;
    }

    free(source);
    return 0;
}

static int cmd_ast(const char *path) {
    char *source = read_file(path);
    if (!source) return 1;

    Lexer lex;
    lexer_init(&lex, source);

    Parser parser;
    parser_init(&parser, &lex);

    AstNode *program = parser_parse_program(&parser);
    if (!program || parser_had_error(&parser)) {
        /* The parser reports its own errors to stderr. */
        free(source);
        return 1;
    }

    ast_dump(program, stdout, 0);
    /* AST is leaked intentionally; the process exits next. */
    free(source);
    return 0;
}

static int cmd_check(const char *path) {
    char *source = read_file(path);
    if (!source) return 1;

    Lexer lex;
    lexer_init(&lex, source);

    Parser parser;
    parser_init(&parser, &lex);

    AstNode *program = parser_parse_program(&parser);
    if (!program || parser_had_error(&parser)) {
        free(source);
        return 1;
    }

    TypeChecker tc;
    typecheck_init(&tc, stderr);
    int rc = typecheck_program(&tc, program);
    /* AST + types are leaked intentionally; process exits next. */
    free(source);
    return rc;
}

/* `--emit-wasm <file.cv> [-o <out.wasm>]`. With no -o, writes the binary
 * to stdout (handy for `cleavec --emit-wasm x.cv | wasm-validate -`). */
static int cmd_emit_wasm(const char *path, const char *out_path) {
    char *source = read_file(path);
    if (!source) return 1;

    Lexer lex;
    lexer_init(&lex, source);

    Parser parser;
    parser_init(&parser, &lex);

    AstNode *program = parser_parse_program(&parser);
    if (!program || parser_had_error(&parser)) {
        free(source);
        return 1;
    }

    TypeChecker tc;
    typecheck_init(&tc, stderr);
    if (typecheck_program(&tc, program) != 0) {
        free(source);
        return 1;
    }

    Codegen cg;
    cg_init(&cg, stderr);
    WasmBuf bin;
    if (cg_compile_program(&cg, program, &bin) != CG_OK) {
        free(source);
        return 1;
    }

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "error: cannot open '%s' for writing\n", out_path);
            wasm_free(&bin);
            free(source);
            return 1;
        }
    }
    size_t written = fwrite(bin.data, 1, bin.len, out);
    if (out_path) fclose(out);

    int rc = (written == bin.len) ? 0 : 1;
    wasm_free(&bin);
    free(source);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("cleavec %s\n", CLEAVE_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--tokens") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --tokens requires a file argument\n");
            usage(argv[0]);
            return 1;
        }
        return cmd_tokens(argv[2]);
    }

    if (strcmp(argv[1], "--ast") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --ast requires a file argument\n");
            usage(argv[0]);
            return 1;
        }
        return cmd_ast(argv[2]);
    }

    if (strcmp(argv[1], "--check") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --check requires a file argument\n");
            usage(argv[0]);
            return 1;
        }
        return cmd_check(argv[2]);
    }

    if (strcmp(argv[1], "--emit-wasm") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --emit-wasm requires a file argument\n");
            usage(argv[0]);
            return 1;
        }
        const char *src_path = argv[2];
        const char *out_path = NULL;
        for (int i = 3; i < argc - 1; ++i) {
            if (strcmp(argv[i], "-o") == 0) {
                out_path = argv[i + 1];
                break;
            }
        }
        return cmd_emit_wasm(src_path, out_path);
    }

    fprintf(stderr,
            "cleavec %s\n"
            "unknown argument: %s\n",
            CLEAVE_VERSION, argv[1]);
    usage(argv[0]);
    return 2;
}
