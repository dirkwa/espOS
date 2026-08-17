/* SPDX-License-Identifier: Apache-2.0 */
#include "espos_config_priv.h"

static const char k_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t espos_b64_encoded_len(size_t n)
{
    return ((n + 2) / 3) * 4 + 1;
}

size_t espos_b64_encode(const uint8_t *in, size_t n, char *out, size_t out_size)
{
    size_t need = espos_b64_encoded_len(n);
    if (out_size < need) {
        if (out_size) {
            out[0] = '\0';
        }
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= in[i + 2];
        }
        out[o++] = k_alpha[(v >> 18) & 63];
        out[o++] = k_alpha[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? k_alpha[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? k_alpha[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

esp_err_t espos_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len)
{
    /* strip trailing padding */
    while (in_len > 0 && in[in_len - 1] == '=') {
        in_len--;
    }
    if (in_len % 4 == 1) {
        return ESP_ERR_INVALID_ARG; /* impossible length */
    }
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        int v = b64_val(in[i]);
        if (v < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    /* leftover bits must be zero for canonical input; be lenient and ignore */
    *out_len = o;
    return ESP_OK;
}
