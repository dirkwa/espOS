/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include <string.h>

#include "unity.h"
#include "espos_sk_parse.h"

typedef struct {
    int n;
    char path[8][64];
    char value[8][128];
    char meta[8][128];
    char ts[8][40];
    char src[8][32];
    char ctx[8][64];
} acc_t;

static bool collect(const espos_sk_update_t *u, void *arg)
{
    acc_t *a = arg;
    if (a->n < 8) {
        snprintf(a->path[a->n], 64, "%s", u->path);
        snprintf(a->value[a->n], 128, "%s", u->value_json ? u->value_json : "");
        snprintf(a->meta[a->n], 128, "%s", u->meta_json ? u->meta_json : "");
        snprintf(a->ts[a->n], 40, "%s", u->timestamp ? u->timestamp : "");
        snprintf(a->src[a->n], 32, "%s", u->source ? u->source : "");
        snprintf(a->ctx[a->n], 64, "%s", u->context ? u->context : "");
    }
    a->n++;
    return true;
}

TEST_CASE("delta: values, meta, $source, timestamp, context", "[parse]")
{
    const char *d = "{\"context\":\"vessels.urn:mrn:signalk:uuid:abc\",\"updates\":["
                    "{\"$source\":\"n2k.1\",\"timestamp\":\"2026-08-18T10:00:00.000Z\","
                    "\"values\":[{\"path\":\"navigation.speedOverGround\",\"value\":3.2},"
                    "{\"path\":\"navigation.position\",\"value\":{\"latitude\":54.1,\"longitude\":10.2}},"
                    "{\"path\":\"environment.mode\",\"value\":\"night\"},"
                    "{\"path\":\"navigation.anchor.position\",\"value\":null}],"
                    "\"meta\":[{\"path\":\"navigation.speedOverGround\",\"value\":{\"units\":\"m/s\",\"zones\":[]}}]},"
                    "{\"source\":{\"label\":\"derived\"},\"values\":[{\"path\":\"a.b\",\"value\":true}]}]}";
    acc_t a = { 0 };
    espos_sk_frame_t f;
    size_t n = espos_sk_frame_parse(d, strlen(d), &f, collect, &a);
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_DELTA, f.kind);
    TEST_ASSERT_EQUAL(6, n);
    TEST_ASSERT_EQUAL_STRING("navigation.speedOverGround", a.path[0]);
    TEST_ASSERT_EQUAL_STRING("3.2", a.value[0]);
    TEST_ASSERT_EQUAL_STRING("n2k.1", a.src[0]);
    TEST_ASSERT_EQUAL_STRING("2026-08-18T10:00:00.000Z", a.ts[0]);
    TEST_ASSERT_EQUAL_STRING("vessels.urn:mrn:signalk:uuid:abc", a.ctx[0]);
    TEST_ASSERT_EQUAL_STRING("{\"latitude\":54.1,\"longitude\":10.2}", a.value[1]);
    TEST_ASSERT_EQUAL_STRING("\"night\"", a.value[2]);
    TEST_ASSERT_EQUAL_STRING("null", a.value[3]);
    TEST_ASSERT_EQUAL_STRING("navigation.speedOverGround", a.path[4]);
    TEST_ASSERT_EQUAL_STRING("", a.value[4]);
    TEST_ASSERT_EQUAL_STRING("{\"units\":\"m/s\",\"zones\":[]}", a.meta[4]);
    TEST_ASSERT_EQUAL_STRING("derived", a.src[5]);
    TEST_ASSERT_EQUAL_STRING("true", a.value[5]);
    espos_sk_frame_free(&f);
}

TEST_CASE("hello, response, error and garbage frames", "[parse]")
{
    espos_sk_frame_t f;
    acc_t a = { 0 };
    const char *h = "{\"name\":\"signalk-server\",\"version\":\"2.31.1\",\"self\":\"vessels.urn:x\",\"roles\":[\"master\"]}";
    TEST_ASSERT_EQUAL(0, espos_sk_frame_parse(h, strlen(h), &f, collect, &a));
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_HELLO, f.kind);
    TEST_ASSERT_EQUAL_STRING("vessels.urn:x", f.self);
    espos_sk_frame_free(&f);
    const char *r = "{\"requestId\":\"1234\",\"state\":\"COMPLETED\",\"statusCode\":200}";
    espos_sk_frame_parse(r, strlen(r), &f, collect, &a);
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_RESPONSE, f.kind);
    TEST_ASSERT_EQUAL_STRING("1234", f.request_id);
    TEST_ASSERT_EQUAL_STRING("COMPLETED", f.state);
    TEST_ASSERT_EQUAL(200, f.status_code);
    espos_sk_frame_free(&f);
    const char *fail = "{\"requestId\":\"1\",\"state\":\"FAILED\",\"statusCode\":405,\"message\":\"no handler\"}";
    espos_sk_frame_parse(fail, strlen(fail), &f, collect, &a);
    TEST_ASSERT_EQUAL(405, f.status_code);
    TEST_ASSERT_EQUAL_STRING("no handler", f.message);
    espos_sk_frame_free(&f);
    const char *e = "{\"errorMessage\":\"unauthorized\"}";
    espos_sk_frame_parse(e, strlen(e), &f, collect, &a);
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_ERROR, f.kind);
    TEST_ASSERT_EQUAL_STRING("unauthorized", f.error);
    espos_sk_frame_free(&f);
    TEST_ASSERT_EQUAL(0, espos_sk_frame_parse("[1,2", 4, &f, collect, &a));
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_UNKNOWN, f.kind);
    espos_sk_frame_free(&f);
    TEST_ASSERT_EQUAL(0, a.n);
}

TEST_CASE("path patterns", "[parse]")
{
    TEST_ASSERT_TRUE(espos_sk_path_matches("navigation.speedOverGround", "navigation.speedOverGround"));
    TEST_ASSERT_FALSE(espos_sk_path_matches("navigation.speedOverGround", "navigation.speedOverGroundX"));
    TEST_ASSERT_TRUE(espos_sk_path_matches("notifications.*", "notifications.mob"));
    TEST_ASSERT_TRUE(espos_sk_path_matches("notifications.*", "notifications.anchor.dragging"));
    TEST_ASSERT_TRUE(espos_sk_path_matches("notifications.*", "notifications"));
    TEST_ASSERT_FALSE(espos_sk_path_matches("notifications.*", "notificationsX.y"));
    TEST_ASSERT_TRUE(espos_sk_path_matches("environment*", "environment.wind.speedApparent"));
    TEST_ASSERT_TRUE(espos_sk_path_matches("*", "anything.at.all"));
    TEST_ASSERT_FALSE(espos_sk_path_matches("", "a"));
}
