#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cleave.h"
#include "lexer.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "usage:\n"
            "  %s --version\n"
            "  %s --help\n"
            "  %s --tokens <file.cv>\n",
            prog, prog, prog);
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

    fprintf(stderr,
            "cleavec %s\n"
            "unknown argument: %s\n",
            CLEAVE_VERSION, argv[1]);
    usage(argv[0]);
    return 2;
}
