#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cleave.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--version] [--help] <file.cv>\n",
            prog);
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

    fprintf(stderr,
            "cleavec %s\n"
            "compiler is not implemented yet; this binary is a placeholder\n"
            "see https://github.com/cleave-lang/cleave for the roadmap\n",
            CLEAVE_VERSION);
    return 2;
}
