/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host harness: config store on emulated NVS + real REST server. The port
 * comes from ESPOS_TEST_PORT (default 18080). run_test.py drives it.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_private/partition_linux.h"
#include "nvs_flash.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_httpd.h"
#include "espos_wifi.h"

static const char *TAG = "harness";
static volatile sig_atomic_t s_terminate;

static void on_sigterm(int sig)
{
    (void)sig;
    s_terminate = 1;
}

/* Remove the emulated-flash temp file. Runs on esp_restart() (shutdown
 * handler) and on SIGTERM from the runner. */
static void cleanup_flash_file(void)
{
    esp_partition_get_file_mmap_ctrl_input()->remove_dump = true;
    esp_partition_file_munmap();
}

static void on_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "config changed: %s.%s", ns, key);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0); /* the runner reads us through a pipe */
    signal(SIGTERM, on_sigterm);
    signal(SIGINT, on_sigterm);
    /* A write to a socket whose peer went away raises SIGPIPE on Linux (no
     * such thing on lwIP); without this the harness dies silently the first
     * time an SSE client disconnects. */
    signal(SIGPIPE, SIG_IGN);
    esp_register_shutdown_handler(cleanup_flash_file);
    const char *port_env = getenv("ESPOS_TEST_PORT");
    int port = port_env ? atoi(port_env) : 18080;
    if (getenv("ESPOS_TEST_FRESH")) {
        (void)nvs_flash_erase_partition("nvs");
    }
    ESP_ERROR_CHECK(espos_config_init(NULL, NULL));
    ESP_ERROR_CHECK(espos_config_subscribe(on_change, NULL));
    /* The harness overrides the configured port so the runner can pick one. */
    ESP_ERROR_CHECK(espos_config_set_i32(ESPOS_CFG_NS_HTTPD, ESPOS_CFG_HTTPD_PORT, port));
    ESP_ERROR_CHECK(espos_httpd_start());
    ESP_ERROR_CHECK(espos_wifi_start()); /* simulated driver on the host, see port_sim.c */
    /* Announce readiness with a raw write loop: stdio gives up on EINTR
     * (which the simulator's tick signals can cause) and would silently drop
     * the line. */
    char ready[64];
    int n = snprintf(ready, sizeof(ready), "ESPOS_HARNESS_READY port=%d\n", port);
    fflush(stdout);
    for (int off = 0; off < n;) {
        ssize_t w = write(STDOUT_FILENO, ready + off, (size_t)(n - off));
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }
        off += (int)w;
    }
    while (!s_terminate) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    cleanup_flash_file();
    exit(0);
}
