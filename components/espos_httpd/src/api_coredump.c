/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/system/coredump — the last crash, from the coredump partition.
 *
 *   GET            summary JSON (404 when there is none)
 *   GET  /raw      the raw image (application/octet-stream) for
 *                  `espcoredump.py info_corefile -c <file> -t raw build/espos.elf`
 *   DELETE         erase it
 *   POST /api/v1/system/crash   deliberate crash (CONFIG_ESPOS_HTTPD_DEBUG_CRASH only)
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_core_dump.h"
#include "esp_partition.h"
#endif

#include "espos_httpd.h"
#include "espos_httpd_priv.h"

#if !CONFIG_IDF_TARGET_LINUX && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

static const char *TAG = "espos_httpd";

static bool have_dump(size_t *addr, size_t *size)
{
    return esp_core_dump_image_get(addr, size) == ESP_OK && *size > 0;
}

static esp_err_t coredump_get(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    if (!have_dump(&addr, &size)) {
        return espos_httpd_send_error(req, "404 Not Found", "not_found", "no core dump stored");
    }
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (!sum) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t err = esp_core_dump_get_summary(sum);
    char *body = malloc(1536);
    if (!body) {
        free(sum);
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    int n = snprintf(body, 1536, "{\"present\":true,\"size\":%u,\"valid\":%s",
                     (unsigned)size, esp_core_dump_image_check() == ESP_OK ? "true" : "false");
    if (err == ESP_OK) {
        char task[17];
        memcpy(task, sum->exc_task, 16);
        task[16] = '\0';
        char sha[APP_ELF_SHA256_SZ + 1];
        memcpy(sha, sum->app_elf_sha256, APP_ELF_SHA256_SZ);
        sha[APP_ELF_SHA256_SZ] = '\0';
        n += snprintf(body + n, 1536 - n, ",\"task\":\"%s\",\"pc\":\"0x%08" PRIx32 "\",\"app_elf_sha256\":\"%s\",\"version\":%" PRIu32,
                      task, sum->exc_pc, sha, sum->core_dump_version);
#if __XTENSA__
        n += snprintf(body + n, 1536 - n, ",\"exc_cause\":%" PRIu32 ",\"exc_vaddr\":\"0x%08" PRIx32 "\",\"backtrace_corrupted\":%s,\"backtrace\":[",
                      sum->ex_info.exc_cause, sum->ex_info.exc_vaddr, sum->exc_bt_info.corrupted ? "true" : "false");
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16 && n < 1400; i++) {
            n += snprintf(body + n, 1536 - n, "%s\"0x%08" PRIx32 "\"", i ? "," : "", sum->exc_bt_info.bt[i]);
        }
        n += snprintf(body + n, 1536 - n, "]");
#else
        n += snprintf(body + n, 1536 - n, ",\"mcause\":%" PRIu32 ",\"mtval\":\"0x%08" PRIx32 "\",\"ra\":\"0x%08" PRIx32 "\",\"sp\":\"0x%08" PRIx32 "\",\"stackdump_bytes\":%" PRIu32,
                      sum->ex_info.mcause, sum->ex_info.mtval, sum->ex_info.ra, sum->ex_info.sp, sum->exc_bt_info.dump_size);
#endif
    } else {
        n += snprintf(body + n, 1536 - n, ",\"summary_error\":\"%s\"", esp_err_to_name(err));
    }
    snprintf(body + n, 1536 - n, "}");
    free(sum);
    esp_err_t r = espos_httpd_send_json(req, NULL, body);
    free(body);
    return r;
}

static esp_err_t coredump_raw_get(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    if (!have_dump(&addr, &size)) {
        return espos_httpd_send_error(req, "404 Not Found", "not_found", "no core dump stored");
    }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_partition", "coredump partition missing");
    }
    /* addr is absolute flash address; read relative to the partition. */
    size_t off = addr >= part->address ? addr - part->address : 0;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char *buf = malloc(1024);
    if (!buf) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t err = ESP_OK;
    for (size_t done = 0; done < size && err == ESP_OK; done += 1024) {
        size_t n = size - done < 1024 ? size - done : 1024;
        err = esp_partition_read(part, off + done, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, n);
        }
    }
    free(buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t coredump_delete(httpd_req_t *req)
{
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "erase_failed", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "core dump erased");
    return espos_httpd_send_json(req, NULL, "{\"status\":\"erased\"}");
}

#else /* host or coredump disabled */

static esp_err_t coredump_get(httpd_req_t *req)
{
    return espos_httpd_send_error(req, "404 Not Found", "not_found", "no core dump stored");
}
static esp_err_t coredump_raw_get(httpd_req_t *req)
{
    return coredump_get(req);
}
static esp_err_t coredump_delete(httpd_req_t *req)
{
    return espos_httpd_send_json(req, NULL, "{\"status\":\"erased\"}");
}

#endif

#if CONFIG_ESPOS_HTTPD_DEBUG_CRASH
#if CONFIG_IDF_TARGET_LINUX || !CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
static const char *TAG = "espos_httpd";
#endif
static esp_err_t crash_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"crashing\"}");
    ESP_LOGE(TAG, "deliberate crash requested");
    volatile int *p = (volatile int *)0x00000010;
    *p = 42;                    /* store to an unmapped address → panic → core dump */
    return ESP_OK;
}
#endif

esp_err_t espos_httpd_register_coredump_api(httpd_handle_t h)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/system/coredump", .method = HTTP_GET, .handler = coredump_get },
        { .uri = "/api/v1/system/coredump/raw", .method = HTTP_GET, .handler = coredump_raw_get },
        { .uri = "/api/v1/system/coredump", .method = HTTP_DELETE, .handler = coredump_delete },
#if CONFIG_ESPOS_HTTPD_DEBUG_CRASH
        { .uri = "/api/v1/system/crash", .method = HTTP_POST, .handler = crash_post },
#endif
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(h, &uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
