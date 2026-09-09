/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sk_frame_parse() — every byte this sees came off the network.
 *
 * This is the parser with the shortest path from a hostile input to the
 * device: a Signal K server (or anything that has taken its place) sends a
 * text frame, and this turns it into paths, values and PUT requests. It is
 * reached before authentication means anything, because the frame arrives on
 * a socket the device opened itself.
 *
 * The harness drives the documented contract, parse then free, because a leak
 * on a malformed frame is as real a defect as a crash: a device that leaks a
 * few hundred bytes per bad frame dies in a week, on the water, and looks like
 * a memory problem rather than a parsing one.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "espos_sk_parse.h"

/* Touch every field the parser filled. Reading them is the point: a pointer
 * that survived into the callback but was never valid is a use-after-free the
 * sanitizer sees only if something dereferences it. */
static bool on_update(const espos_sk_update_t *u, void *arg)
{
    size_t *n = (size_t *)arg;
    volatile size_t sink = 0;

    if (u->path) {
        sink += strlen(u->path);
    }
    if (u->value_json) {
        sink += strlen(u->value_json);
    }
    if (u->request_id) {
        sink += strlen(u->request_id);
    }
    (void)sink;

    /* Stop after a bound so a frame with a million items does not turn the
     * fuzzer into a benchmark. The parser's own limits are what we are
     * testing, not how fast it can iterate. */
    return ++(*n) < 4096;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* The parser takes a pointer and a length, so it must not need a
     * terminator -- but pass an exact-sized copy anyway. A heap buffer that
     * ends precisely at `size` is what turns a one-byte overread into an ASan
     * report instead of a silent read of whatever followed in the fuzzer's
     * own memory. */
    char *buf = (char *)malloc(size ? size : 1);
    if (!buf) {
        return 0;
    }
    memcpy(buf, data, size);

    espos_sk_frame_t info;
    memset(&info, 0, sizeof(info));
    size_t items = 0;

    (void)espos_sk_frame_parse(buf, size, &info, on_update, &items);
    espos_sk_frame_free(&info);

    /* Freeing twice must not be a double free: the stream task does exactly
     * this on some error paths, and the contract says free is idempotent
     * because `info` is zeroed by it. */
    espos_sk_frame_free(&info);

    free(buf);
    return 0;
}
