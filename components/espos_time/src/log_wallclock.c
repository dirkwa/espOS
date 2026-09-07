/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The strong definitions of two hooks that other components declare weakly.
 *
 * espos_log and espos_httpd both want the wall clock — one to stamp a stored
 * line, the other to put a `time` object in /system/info — and neither may
 * depend on espos_time: espos_time already depends on espos_httpd for its own
 * endpoints, and espos_log sits below everything by design so that logging a
 * line never drags a component in. A build-time dependency either way round
 * closes a cycle.
 *
 * So the direction is inverted. Each of those components declares a weak
 * function that answers "there is no clock", and this file — compiled only
 * when espos_time is in the build — overrides it with the real one. A firmware
 * without espos_time links the weak default and behaves exactly as it did
 * before this component existed; one with it gets the clock, with no component
 * having to name another.
 *
 * The alternative, a runtime registration call from espos_time into each
 * consumer, would need espos_time to know both of them by name and would put
 * a function pointer in the log's hot path. This costs nothing at run time and
 * nothing in the dependency graph.
 */
#include <stddef.h>

#include "espos_time.h"

/* espos_log's hook (components/espos_log/src/espos_log.c). Called with the
 * ring lock held, once per stored line: it must not allocate, block or log. */
size_t espos_log_wallclock_hook(char *buf, size_t n)
{
    if (!espos_time_is_synced()) {
        return 0; /* an unsynced clock stamps nothing; the line is unchanged */
    }
    return espos_time_iso8601(buf, n);
}

/* espos_httpd's hook (components/espos_httpd/src/api_system.c), for the
 * "time" object of GET /api/v1/system/info. */
bool espos_httpd_wallclock_hook(bool *synced, const char **source, int64_t *unix_ms)
{
    *synced = espos_time_is_synced();
    *source = espos_time_src_str(espos_time_source());
    *unix_ms = espos_time_now_ms();
    return true;
}
