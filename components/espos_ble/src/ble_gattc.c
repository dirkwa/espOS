/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Bluedroid GATT client - see ble_gattc.h.
 *
 * Connection state is a fixed array indexed by slot, and every public call
 * takes the conn_id it applies to. That is deliberate: the implementation this
 * replaces ignored the connection handle and scanned all connections for the
 * first characteristic whose UUID matched, so two identical BMSs (which share
 * a vendor UUID) silently crossed wires.
 */

#include "sdkconfig.h"

#ifdef CONFIG_BT_GATTC_ENABLE

#include "ble_gattc.h"

#include <string.h>

#include "ble_proto.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"

static const char *TAG = "espos_ble_gattc";

/* Client Characteristic Configuration Descriptor; writing 0x0001 to it is what
 * actually turns notifications on, over and above register_for_notify(). */
static const uint16_t kCccdUuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t kNotifyEnable[2] = {0x01, 0x00};

#define MAX_CHARS 16

typedef struct {
    uint8_t uuid[16];
    uint16_t handle;
} char_entry_t;

typedef struct {
    bool in_use;
    bool connected;
    uint16_t conn_id;
    esp_bd_addr_t bda;
    char mac[ESPOS_BLE_ADDR_LEN];
    uint8_t service_uuid[16];
    bool has_service_filter;
    uint16_t svc_start, svc_end;
    bool service_found;
    char_entry_t chars[MAX_CHARS];
    size_t char_count;
} conn_slot_t;

static conn_slot_t s_slots[ESPOS_BLE_GATTC_MAX_CONN];
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static espos_ble_callbacks_t s_cb;

static void format_bda(const esp_bd_addr_t bda, char *out)
{
    snprintf(out, ESPOS_BLE_ADDR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool parse_mac(const char *mac, esp_bd_addr_t out)
{
    unsigned b[6];
    if (sscanf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

/* Bluedroid keeps 128-bit UUIDs little-endian; ble_proto works big-endian. */
static void uuid_from_bt(const esp_bt_uuid_t *in, uint8_t out[16])
{
    char text[ESPOS_BLE_UUID_MAX];
    if (in->len == ESP_UUID_LEN_16) {
        snprintf(text, sizeof(text), "%04x", in->uuid.uuid16);
    } else if (in->len == ESP_UUID_LEN_32) {
        snprintf(text, sizeof(text), "%08" PRIx32, in->uuid.uuid32);
    } else {
        const uint8_t *u = in->uuid.uuid128;
        snprintf(text, sizeof(text),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                 "%02x%02x%02x%02x%02x%02x",
                 u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8], u[7],
                 u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
    }
    espos_ble_uuid_parse(text, out, NULL);
}

static void uuid_to_bt(const uint8_t in[16], esp_bt_uuid_t *out)
{
    out->len = ESP_UUID_LEN_128;
    for (int i = 0; i < 16; i++) out->uuid.uuid128[i] = in[15 - i];
}

static conn_slot_t *slot_by_conn_id(uint16_t conn_id)
{
    for (size_t i = 0; i < ESPOS_BLE_GATTC_MAX_CONN; i++) {
        if (s_slots[i].in_use && s_slots[i].connected &&
            s_slots[i].conn_id == conn_id) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static conn_slot_t *slot_by_bda(const esp_bd_addr_t bda)
{
    for (size_t i = 0; i < ESPOS_BLE_GATTC_MAX_CONN; i++) {
        if (s_slots[i].in_use && memcmp(s_slots[i].bda, bda, 6) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static uint16_t handle_for_uuid(const conn_slot_t *s, const char *uuid_str)
{
    uint8_t want[16];
    if (!espos_ble_uuid_parse(uuid_str, want, NULL)) return 0;
    for (size_t i = 0; i < s->char_count; i++) {
        if (espos_ble_uuid_equal(s->chars[i].uuid, want)) return s->chars[i].handle;
    }
    return 0;
}

static void slot_release(conn_slot_t *s)
{
    memset(s, 0, sizeof(*s));
}

esp_err_t espos_ble_gattc_init(const espos_ble_callbacks_t *cb)
{
    if (cb) s_cb = *cb;
    memset(s_slots, 0, sizeof(s_slots));
    /* The app owns the Bluedroid GATTC callback registration and forwards
     * events to espos_ble_gattc_event(); registering here too would fight it. */
    esp_err_t err = esp_ble_gattc_app_register(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_register: %s", esp_err_to_name(err));
    }
    return err;
}

int espos_ble_gatt_connect(const char *mac, const char *service_uuid)
{
    if (!mac) return -1;

    conn_slot_t *s = NULL;
    for (size_t i = 0; i < ESPOS_BLE_GATTC_MAX_CONN; i++) {
        if (!s_slots[i].in_use) { s = &s_slots[i]; break; }
    }
    if (!s) {
        ESP_LOGW(TAG, "no free connection slot (max %d)", ESPOS_BLE_GATTC_MAX_CONN);
        return -1;
    }

    memset(s, 0, sizeof(*s));
    if (!parse_mac(mac, s->bda)) {
        ESP_LOGE(TAG, "bad MAC: %s", mac);
        return -1;
    }
    snprintf(s->mac, sizeof(s->mac), "%s", mac);
    if (service_uuid && service_uuid[0] &&
        espos_ble_uuid_parse(service_uuid, s->service_uuid, NULL)) {
        s->has_service_filter = true;
    }
    s->in_use = true;

    esp_err_t err = esp_ble_gattc_open(s_gattc_if, s->bda, BLE_ADDR_TYPE_PUBLIC, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gattc_open(%s): %s", mac, esp_err_to_name(err));
        slot_release(s);
        return -1;
    }
    /* conn_id is not known until OPEN_EVT; the slot index stands in as the
     * caller-visible handle until then. */
    return (int)(s - s_slots);
}

static conn_slot_t *slot_by_handle(int conn_handle)
{
    if (conn_handle < 0 || conn_handle >= ESPOS_BLE_GATTC_MAX_CONN) return NULL;
    conn_slot_t *s = &s_slots[conn_handle];
    return s->in_use ? s : NULL;
}

esp_err_t espos_ble_gatt_subscribe(int conn_handle, const char *char_uuid)
{
    conn_slot_t *s = slot_by_handle(conn_handle);
    if (!s || !s->connected) return ESP_ERR_INVALID_STATE;
    uint16_t handle = handle_for_uuid(s, char_uuid);
    if (!handle) {
        ESP_LOGW(TAG, "subscribe: unknown characteristic %s", char_uuid);
        return ESP_ERR_NOT_FOUND;
    }

    /* One call, with a real BDA. The code this replaces called this twice -
     * first with a NULL bda into an API that dereferences it. */
    esp_err_t err = esp_ble_gattc_register_for_notify(s_gattc_if, s->bda, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_for_notify(%s): %s", char_uuid, esp_err_to_name(err));
        return err;
    }

    /* And enable them at the peripheral by writing the CCCD. */
    uint16_t count = 1;
    esp_gattc_descr_elem_t descr;
    esp_bt_uuid_t cccd = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = kCccdUuid}};
    esp_gatt_status_t st = esp_ble_gattc_get_descr_by_char_handle(
        s_gattc_if, s->conn_id, handle, cccd, &descr, &count);
    if (st == ESP_GATT_OK && count > 0) {
        esp_ble_gattc_write_char_descr(s_gattc_if, s->conn_id, descr.handle,
                                       sizeof(kNotifyEnable),
                                       (uint8_t *)kNotifyEnable,
                                       ESP_GATT_WRITE_TYPE_RSP,
                                       ESP_GATT_AUTH_REQ_NONE);
    } else {
        ESP_LOGW(TAG, "no CCCD for %s - notifications may not start", char_uuid);
    }
    return ESP_OK;
}

esp_err_t espos_ble_gatt_read(int conn_handle, const char *char_uuid)
{
    conn_slot_t *s = slot_by_handle(conn_handle);
    if (!s || !s->connected) return ESP_ERR_INVALID_STATE;
    uint16_t handle = handle_for_uuid(s, char_uuid);
    if (!handle) return ESP_ERR_NOT_FOUND;
    return esp_ble_gattc_read_char(s_gattc_if, s->conn_id, handle,
                                   ESP_GATT_AUTH_REQ_NONE);
}

esp_err_t espos_ble_gatt_write(int conn_handle, const char *char_uuid,
                               const uint8_t *data, size_t len,
                               espos_ble_write_mode_t mode)
{
    conn_slot_t *s = slot_by_handle(conn_handle);
    if (!s || !s->connected) return ESP_ERR_INVALID_STATE;
    uint16_t handle = handle_for_uuid(s, char_uuid);
    if (!handle) {
        ESP_LOGW(TAG, "write: unknown characteristic %s", char_uuid);
        return ESP_ERR_NOT_FOUND;
    }

    /* The reason this parameter exists: peripherals such as JK-BMS and
     * Daly-BMS reject write-with-response on their command characteristic
     * ("Write not permitted") and accept only write-without-response.
     * NO_RSP produces no WRITE_CHAR_EVT, so callers must not await one. */
    esp_gatt_write_type_t wt = (mode == ESPOS_BLE_WRITE_NO_RESPONSE)
                                   ? ESP_GATT_WRITE_TYPE_NO_RSP
                                   : ESP_GATT_WRITE_TYPE_RSP;

    esp_err_t err = esp_ble_gattc_write_char(s_gattc_if, s->conn_id, handle,
                                             len, (uint8_t *)data, wt,
                                             ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write_char(%s): %s", char_uuid, esp_err_to_name(err));
        return err;
    }

    /* A no-response write produces no WRITE_CHAR_EVT, so the caller must not
     * wait for one. Report that through the return value rather than by
     * invoking on_gatt_write_done() from inside this call: the caller holds
     * its session lock across this function, and a synchronous callback would
     * re-enter it on a non-recursive mutex.
     *
     * ESP_ERR_NOT_FINISHED means "sent, and no completion is coming". */
    return (mode == ESPOS_BLE_WRITE_NO_RESPONSE) ? ESP_ERR_NOT_FINISHED : ESP_OK;
}

esp_err_t espos_ble_gatt_disconnect(int conn_handle)
{
    conn_slot_t *s = slot_by_handle(conn_handle);
    if (!s) return ESP_ERR_INVALID_STATE;
    if (s->connected) return esp_ble_gattc_close(s_gattc_if, s->conn_id);
    slot_release(s);
    return ESP_OK;
}

uint32_t espos_ble_gatt_active_count(void)
{
    uint32_t n = 0;
    for (size_t i = 0; i < ESPOS_BLE_GATTC_MAX_CONN; i++) {
        if (s_slots[i].in_use && s_slots[i].connected) n++;
    }
    return n;
}

static void discover_characteristics(conn_slot_t *s)
{
    uint16_t count = 0;
    esp_gatt_status_t st = esp_ble_gattc_get_attr_count(
        s_gattc_if, s->conn_id, ESP_GATT_DB_CHARACTERISTIC,
        s->svc_start, s->svc_end, 0, &count);
    if (st != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TAG, "no characteristics in service");
        if (s_cb.on_gatt_error) {
            s_cb.on_gatt_error((int)(s - s_slots), "no characteristics", s_cb.arg);
        }
        return;
    }
    if (count > MAX_CHARS) count = MAX_CHARS;

    esp_gattc_char_elem_t elems[MAX_CHARS];
    uint16_t got = count;
    st = esp_ble_gattc_get_all_char(s_gattc_if, s->conn_id, s->svc_start,
                                    s->svc_end, elems, &got, 0);
    if (st != ESP_GATT_OK) {
        ESP_LOGE(TAG, "get_all_char: %d", st);
        if (s_cb.on_gatt_error) {
            s_cb.on_gatt_error((int)(s - s_slots), "characteristic discovery failed",
                               s_cb.arg);
        }
        return;
    }

    s->char_count = 0;
    for (uint16_t i = 0; i < got && s->char_count < MAX_CHARS; i++) {
        uuid_from_bt(&elems[i].uuid, s->chars[s->char_count].uuid);
        s->chars[s->char_count].handle = elems[i].char_handle;
        s->char_count++;
    }
    ESP_LOGI(TAG, "%s: %u characteristics", s->mac, (unsigned)s->char_count);

    if (s_cb.on_gatt_connected) {
        s_cb.on_gatt_connected((int)(s - s_slots), s_cb.arg);
    }
}

void espos_ble_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) {
            s_gattc_if = gattc_if;
            ESP_LOGI(TAG, "GATTC registered (if=%d)", gattc_if);
        } else {
            ESP_LOGE(TAG, "GATTC register failed: %d", param->reg.status);
        }
        break;

    case ESP_GATTC_OPEN_EVT: {
        conn_slot_t *s = slot_by_bda(param->open.remote_bda);
        if (!s) break;
        if (param->open.status != ESP_GATT_OK) {
            char mac[ESPOS_BLE_ADDR_LEN];
            format_bda(param->open.remote_bda, mac);
            ESP_LOGE(TAG, "open(%s) failed: %d", mac, param->open.status);
            int h = (int)(s - s_slots);
            /* Same ordering as DISCONNECT_EVT: keep the slot reserved until
             * the callback has resolved this handle. */
            s->connected = false;
            if (s_cb.on_gatt_error) s_cb.on_gatt_error(h, "connect failed", s_cb.arg);
            slot_release(s);
            break;
        }
        s->conn_id = param->open.conn_id;
        s->connected = true;
        esp_ble_gattc_send_mtu_req(gattc_if, s->conn_id);
        break;
    }

    case ESP_GATTC_CFG_MTU_EVT: {
        conn_slot_t *s = slot_by_conn_id(param->cfg_mtu.conn_id);
        if (!s) break;
        if (s->has_service_filter) {
            esp_bt_uuid_t filter;
            uuid_to_bt(s->service_uuid, &filter);
            esp_ble_gattc_search_service(gattc_if, s->conn_id, &filter);
        } else {
            esp_ble_gattc_search_service(gattc_if, s->conn_id, NULL);
        }
        break;
    }

    case ESP_GATTC_SEARCH_RES_EVT: {
        conn_slot_t *s = slot_by_conn_id(param->search_res.conn_id);
        if (!s) break;
        /* Exact 128-bit comparison. The old code substring-matched UUID text,
         * which could latch onto the wrong service. */
        uint8_t found[16];
        uuid_from_bt(&param->search_res.srvc_id.uuid, found);
        if (!s->has_service_filter || espos_ble_uuid_equal(found, s->service_uuid)) {
            s->svc_start = param->search_res.start_handle;
            s->svc_end = param->search_res.end_handle;
            s->service_found = true;
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        conn_slot_t *s = slot_by_conn_id(param->search_cmpl.conn_id);
        if (!s) break;
        if (!s->service_found) {
            ESP_LOGW(TAG, "%s: requested service not found", s->mac);
            if (s_cb.on_gatt_error) {
                s_cb.on_gatt_error((int)(s - s_slots), "service not found", s_cb.arg);
            }
            break;
        }
        discover_characteristics(s);
        break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
        conn_slot_t *s = slot_by_conn_id(param->notify.conn_id);
        if (!s || !s_cb.on_gatt_notify) break;
        for (size_t i = 0; i < s->char_count; i++) {
            if (s->chars[i].handle != param->notify.handle) continue;
            char uuid[ESPOS_BLE_UUID_MAX];
            espos_ble_uuid_format(s->chars[i].uuid, 128, uuid);
            s_cb.on_gatt_notify((int)(s - s_slots), uuid, param->notify.value,
                                param->notify.value_len, s_cb.arg);
            break;
        }
        break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
        conn_slot_t *s = slot_by_conn_id(param->read.conn_id);
        if (!s || !s_cb.on_gatt_read || param->read.status != ESP_GATT_OK) break;
        for (size_t i = 0; i < s->char_count; i++) {
            if (s->chars[i].handle != param->read.handle) continue;
            char uuid[ESPOS_BLE_UUID_MAX];
            espos_ble_uuid_format(s->chars[i].uuid, 128, uuid);
            s_cb.on_gatt_read((int)(s - s_slots), uuid, param->read.value,
                              param->read.value_len, s_cb.arg);
            break;
        }
        break;
    }

    /* Only characteristic writes advance an init sequence. Descriptor writes
     * (the CCCD enable) share this callback in the code this replaces, and a
     * CCCD completion could spuriously advance the sequence by one. */
    case ESP_GATTC_WRITE_CHAR_EVT: {
        conn_slot_t *s = slot_by_conn_id(param->write.conn_id);
        if (!s || !s_cb.on_gatt_write_done) break;
        for (size_t i = 0; i < s->char_count; i++) {
            if (s->chars[i].handle != param->write.handle) continue;
            char uuid[ESPOS_BLE_UUID_MAX];
            espos_ble_uuid_format(s->chars[i].uuid, 128, uuid);
            s_cb.on_gatt_write_done((int)(s - s_slots), uuid,
                                    param->write.status == ESP_GATT_OK, s_cb.arg);
            break;
        }
        break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "CCCD write failed: %d", param->write.status);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT: {
        conn_slot_t *s = slot_by_conn_id(param->disconnect.conn_id);
        if (!s) break;
        int h = (int)(s - s_slots);
        int reason = param->disconnect.reason;
        ESP_LOGI(TAG, "%s disconnected (reason %d)", s->mac, reason);
        /* Mark it closed but keep the slot reserved until the callback has
         * run. Releasing first would let a new connect claim the same slot
         * index - which is the caller's conn_handle - while the gateway is
         * still resolving the old one, so a stale event could land on a
         * freshly created session. */
        s->connected = false;
        if (s_cb.on_gatt_disconnected) s_cb.on_gatt_disconnected(h, reason, s_cb.arg);
        slot_release(s);
        break;
    }

    default:
        break;
    }
}

#endif /* CONFIG_BT_GATTC_ENABLE */
