/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * A main() for machines without clang.
 *
 * libFuzzer supplies its own main and drives the harness with generated
 * input. gcc has no libFuzzer, so this replays a corpus directory through
 * the same LLVMFuzzerTestOneInput() under ASan and UBSan.
 *
 * That is a regression check, not fuzzing: it re-runs what the corpus already
 * knows, which is exactly what you want after a parser change and is worth
 * having in the host suite. It discovers nothing new. CI installs clang and
 * gets the real thing.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
#else
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
#endif

static int run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0; /* not a readable file: skip, not a failure */
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);

    LLVMFuzzerTestOneInput(buf, got);
    free(buf);
    return 1;
}

int main(int argc, char **argv)
{
    /* Always exercise the empty input: a parser that assumes at least one
     * byte is a common and cheap way to crash, and no corpus file is
     * guaranteed to be empty. */
    LLVMFuzzerTestOneInput((const uint8_t *)"", 0);

    int files = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            DIR *d = opendir(argv[i]);
            if (!d) {
                continue;
            }
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') {
                    continue;
                }
                char path[4096];
                if (snprintf(path, sizeof(path), "%s/%s", argv[i], e->d_name) >= (int)sizeof(path)) {
                    continue;
                }
                files += run_file(path);
            }
            closedir(d);
        } else {
            files += run_file(argv[i]);
        }
    }

    printf("replayed %d corpus file(s) + the empty input\n", files);
    return 0;
}
