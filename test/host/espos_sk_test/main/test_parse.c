/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
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
    char req[8][40];
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
        snprintf(a->req[a->n], 40, "%s", u->request_id ? u->request_id : "");
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

/* The frame signalk-server actually writes to a device: src/interfaces/ws.ts
 * handlePut() does spark.write({requestId, context, put: [{path, value}]}).
 * `put` is an ARRAY there -- getting this wrong is the difference between a
 * switch that works from a phone and one that never sees the request. */
TEST_CASE("put: the array form signalk-server sends", "[parse]")
{
    const char *p = "{\"requestId\":\"c0ffee-01\",\"context\":\"vessels.self\","
                    "\"put\":[{\"path\":\"electrical.switches.bilge.state\",\"value\":true}]}";
    acc_t a = { 0 };
    espos_sk_frame_t f;
    size_t n = espos_sk_frame_parse(p, strlen(p), &f, collect, &a);
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_PUT, f.kind);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("c0ffee-01", f.request_id);
    TEST_ASSERT_EQUAL_STRING("electrical.switches.bilge.state", a.path[0]);
    TEST_ASSERT_EQUAL_STRING("true", a.value[0]);
    /* the handler must be able to answer without carrying the frame around */
    TEST_ASSERT_EQUAL_STRING("c0ffee-01", a.req[0]);
    TEST_ASSERT_EQUAL_STRING("vessels.self", a.ctx[0]);
    espos_sk_frame_free(&f);
}

/* The single-object form is what a CLIENT sends (espos_sk_put writes it).
 * Accepting both means the parser does not care which side wrote the frame. */
TEST_CASE("put: the single-object client form, and multiple items", "[parse]")
{
    const char *one = "{\"requestId\":\"r1\",\"put\":{\"path\":\"a.b\",\"value\":1.5}}";
    acc_t a = { 0 };
    espos_sk_frame_t f;
    TEST_ASSERT_EQUAL(1, espos_sk_frame_parse(one, strlen(one), &f, collect, &a));
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_PUT, f.kind);
    TEST_ASSERT_EQUAL_STRING("a.b", a.path[0]);
    TEST_ASSERT_EQUAL_STRING("1.5", a.value[0]);
    TEST_ASSERT_EQUAL_STRING("r1", a.req[0]);
    espos_sk_frame_free(&f);

    /* several paths in one request, and the value types a switch panel uses */
    const char *many = "{\"requestId\":\"r2\",\"put\":["
                       "{\"path\":\"x.one\",\"value\":false},"
                       "{\"path\":\"x.two\",\"value\":\"auto\"},"
                       "{\"path\":\"x.three\",\"value\":null},"
                       "{\"path\":\"x.four\",\"value\":{\"r\":1}}]}";
    acc_t b = { 0 };
    TEST_ASSERT_EQUAL(4, espos_sk_frame_parse(many, strlen(many), &f, collect, &b));
    TEST_ASSERT_EQUAL_STRING("false", b.value[0]);
    TEST_ASSERT_EQUAL_STRING("\"auto\"", b.value[1]);
    /* an explicit null is a real value -- it is how a switch is cleared --
     * and must not be dropped the way a missing key is */
    TEST_ASSERT_EQUAL_STRING("null", b.value[2]);
    TEST_ASSERT_EQUAL_STRING("{\"r\":1}", b.value[3]);
    espos_sk_frame_free(&f);
}

/* A PUT carries a requestId and so does a response; only "put" tells them
 * apart. If this regresses, a device answers its own responses. */
TEST_CASE("put is not confused with a response, and malformed puts", "[parse]")
{
    acc_t a = { 0 };
    espos_sk_frame_t f;
    const char *resp = "{\"requestId\":\"r3\",\"state\":\"COMPLETED\",\"statusCode\":200}";
    TEST_ASSERT_EQUAL(0, espos_sk_frame_parse(resp, strlen(resp), &f, collect, &a));
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_RESPONSE, f.kind);
    espos_sk_frame_free(&f);

    /* an item with no value is not actionable and is skipped, but the frame
     * is still a PUT -- the dispatcher answers it 405 rather than ignoring it */
    const char *nov = "{\"requestId\":\"r4\",\"put\":[{\"path\":\"a.b\"}]}";
    TEST_ASSERT_EQUAL(0, espos_sk_frame_parse(nov, strlen(nov), &f, collect, &a));
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_PUT, f.kind);
    TEST_ASSERT_EQUAL_STRING("r4", f.request_id);
    espos_sk_frame_free(&f);

    /* "put" that is neither array nor object is not a PUT frame at all */
    const char *junk = "{\"requestId\":\"r5\",\"put\":7}";
    TEST_ASSERT_EQUAL(0, espos_sk_frame_parse(junk, strlen(junk), &f, collect, &a));
    TEST_ASSERT_NOT_EQUAL(ESPOS_SK_FRAME_PUT, f.kind);
    espos_sk_frame_free(&f);
    TEST_ASSERT_EQUAL(0, a.n);
}

/* The response shape signalk-server will accept. Its isWsRequestReply()
 * (src/interfaces/ws.ts) requires a string requestId and a state of exactly
 * COMPLETED, PENDING or null; anything else is dropped without a word, and
 * the client then waits out the server's full 60 s timeout. */
TEST_CASE("put response frame shape", "[parse]")
{
    char *f = espos_sk_put_response_frame("abc-123", "COMPLETED", 200, NULL);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("{\"requestId\":\"abc-123\",\"state\":\"COMPLETED\",\"statusCode\":200}", f);
    free(f);

    /* the 405 an unhandled path gets: what signalk-server itself replies */
    f = espos_sk_put_response_frame("r-9", "COMPLETED", 405, "PUT not supported for this path");
    TEST_ASSERT_EQUAL_STRING(
        "{\"requestId\":\"r-9\",\"state\":\"COMPLETED\",\"statusCode\":405,"
        "\"message\":\"PUT not supported for this path\"}",
        f);
    free(f);

    /* PENDING keeps the request alive while a handler finishes the work */
    f = espos_sk_put_response_frame("r-10", "PENDING", 202, NULL);
    TEST_ASSERT_EQUAL_STRING("{\"requestId\":\"r-10\",\"state\":\"PENDING\",\"statusCode\":202}", f);
    free(f);

    /* A quote in the message must not break the frame: an unescaped one
     * makes the response unparseable and the server never resolves the
     * request, which surfaces as a 60 s hang rather than an error. */
    f = espos_sk_put_response_frame("r-11", "COMPLETED", 400, "bad value \"x\"");
    TEST_ASSERT_NOT_NULL(f);
    espos_sk_frame_t info;
    acc_t a = { 0 };
    espos_sk_frame_parse(f, strlen(f), &info, collect, &a);
    TEST_ASSERT_EQUAL(ESPOS_SK_FRAME_RESPONSE, info.kind);
    TEST_ASSERT_EQUAL_STRING("r-11", info.request_id);
    TEST_ASSERT_EQUAL(400, info.status_code);
    TEST_ASSERT_EQUAL_STRING("bad value \"x\"", info.message);
    espos_sk_frame_free(&info);
    free(f);

    TEST_ASSERT_NULL(espos_sk_put_response_frame(NULL, "COMPLETED", 200, NULL));
    TEST_ASSERT_NULL(espos_sk_put_response_frame("r", NULL, 200, NULL));
}
