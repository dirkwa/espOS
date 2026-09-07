/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_httpd authentication policy, driven with a fake port.
 *
 * The properties that matter: nothing is judged while no key is set (unless
 * the build requires one), the portal is always exempt, a key compares
 * whole and only whole, a session lives exactly its lifetime and the table
 * evicts the one idle longest, a new key ends every session, five misses
 * within the window lock every key check out for the lockout time and no
 * fewer do, and a cookie's write needs an Origin whose authority is the
 * Host's. The clock is a number the tests move.
 */
#include <string.h>

#include "espos_httpd_auth_policy.h"
#include "unity.h"

#define KEY   "correct-horse-battery"
#define SLOTS 3

static struct {
    uint32_t now_s;
    uint32_t rnd;
} F;

static uint32_t f_now_s(void *ctx)
{
    (void)ctx;
    return F.now_s;
}

/* Distinct per call and reproducible: every id differs, every run matches. */
static uint32_t f_random(void *ctx)
{
    (void)ctx;
    F.rnd = F.rnd * 1664525u + 1013904223u;
    return F.rnd;
}

static const espos_httpd_auth_port_t PORT = { .now_s = f_now_s, .random = f_random };
static espos_httpd_auth_session_t sessions[SLOTS];
static espos_httpd_auth_policy_t P;

static void fresh(uint32_t ttl_s, bool require_key, const char *key)
{
    memset(&F, 0, sizeof(F));
    F.now_s = 1000;
    F.rnd = 7;
    espos_httpd_auth_policy_init(&P, &PORT, NULL, sessions, SLOTS, ttl_s, require_key);
    espos_httpd_auth_policy_set_key(&P, key);
}

static espos_httpd_auth_verdict_t decide(const espos_httpd_auth_request_t *rq, espos_httpd_auth_method_t *m)
{
    espos_httpd_auth_method_t tmp;
    return espos_httpd_auth_policy_decide(&P, rq, m ? m : &tmp);
}

static void login(char *sid)
{
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_open(&P, sid, ESPOS_HTTPD_AUTH_SID_LEN + 1));
}

/* ------------------------------------------------------------ open device */

TEST_CASE("no key: everything is allowed and nothing is judged", "[auth]")
{
    fresh(3600, false, "");
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_configured(&P));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_required(&P));
    espos_httpd_auth_method_t m;
    espos_httpd_auth_request_t rq = { 0 };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
    /* a client configured with a key against a device that has none yet */
    rq.bearer = "whatever";
    rq.state_changing = true;
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_throttled(&P));
    /* a key check without a key is a miss but not a strike */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, espos_httpd_auth_policy_check_key(&P, "x"));
    TEST_ASSERT_EQUAL(0, P.fail_count);
}

TEST_CASE("no key but the build requires one: 403 everywhere but the portal", "[auth]")
{
    fresh(3600, true, "");
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_required(&P));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_configured(&P));
    espos_httpd_auth_method_t m;
    espos_httpd_auth_request_t rq = { .bearer = "anything" };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNCONFIGURED, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
    rq.from_portal = true;
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_PORTAL, m);
}

/* ------------------------------------------------------------------ bearer */

TEST_CASE("bearer: the whole key or nothing", "[auth]")
{
    fresh(3600, false, KEY);
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_configured(&P));
    espos_httpd_auth_method_t m;
    espos_httpd_auth_request_t rq = { 0 };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
    rq.bearer = KEY;
    rq.state_changing = true; /* no Origin needed for a bearer */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_BEARER, m);
    /* four misses: one short of the lockout, which has a test of its own */
    static const char *misses[] = { "", "correct-horse", "correct-horse-battery-staple", "Correct-horse-battery" };
    for (size_t i = 0; i < sizeof(misses) / sizeof(misses[0]); i++) {
        rq.bearer = misses[i];
        TEST_ASSERT_EQUAL_MESSAGE(ESPOS_HTTPD_AUTH_UNAUTHORIZED, decide(&rq, &m), misses[i]);
        TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
    }
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_throttled(&P));
    rq.bearer = KEY; /* a hit clears the count */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    /* a wrong bearer is refused even next to a live cookie */
    char sid[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    login(sid);
    rq.bearer = "wrong";
    rq.cookie_sid = sid;
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, decide(&rq, &m));
    /* the portal beats every credential */
    rq.from_portal = true;
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_PORTAL, m);
}

/* ---------------------------------------------------------------- sessions */

TEST_CASE("session: 32 hex chars, distinct, valid for its lifetime", "[auth]")
{
    fresh(100, false, KEY);
    char a[ESPOS_HTTPD_AUTH_SID_LEN + 1], b[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    login(a);
    login(b);
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_SID_LEN, strlen(a));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_SID_LEN, strspn(a, "0123456789abcdef"));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
    TEST_ASSERT_EQUAL(2, espos_httpd_auth_policy_sessions_live(&P));
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, a));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, "00000000000000000000000000000000"));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, ""));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, NULL));
    char prefix[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    memcpy(prefix, a, sizeof(prefix));
    prefix[ESPOS_HTTPD_AUTH_SID_LEN - 1] = '\0'; /* one short */
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, prefix));
    F.now_s += 99;
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, a));
    F.now_s += 1; /* exactly the lifetime: gone */
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, a));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, b));
    TEST_ASSERT_EQUAL(0, espos_httpd_auth_policy_sessions_live(&P));
    /* a use does not extend the life: the lifetime is from the login */
    login(a);
    F.now_s += 60;
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, a));
    F.now_s += 40;
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, a));
}

TEST_CASE("session: cookie decision, close, a new key ends them all", "[auth]")
{
    fresh(3600, false, KEY);
    char sid[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    login(sid);
    espos_httpd_auth_method_t m;
    espos_httpd_auth_request_t rq = { .cookie_sid = sid };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_COOKIE, m);
    espos_httpd_auth_policy_session_close(&P, sid);
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
    espos_httpd_auth_policy_session_close(&P, sid); /* twice is fine */
    login(sid);
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    espos_httpd_auth_policy_set_key(&P, "another-key-entirely");
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, decide(&rq, &m));
    TEST_ASSERT_EQUAL(0, espos_httpd_auth_policy_sessions_live(&P));
    /* clearing the key reopens the device; a stale cookie is simply ignored */
    espos_httpd_auth_policy_set_key(&P, NULL);
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
}

TEST_CASE("session table: full evicts the one idle longest", "[auth]")
{
    fresh(3600, false, KEY);
    char a[ESPOS_HTTPD_AUTH_SID_LEN + 1], b[ESPOS_HTTPD_AUTH_SID_LEN + 1], c[ESPOS_HTTPD_AUTH_SID_LEN + 1],
        d[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    login(a);
    F.now_s += 1;
    login(b);
    F.now_s += 1;
    login(c);
    TEST_ASSERT_EQUAL(SLOTS, espos_httpd_auth_policy_sessions_live(&P));
    F.now_s += 1;
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, a)); /* a is now the most recent */
    F.now_s += 1;
    login(d); /* b has been idle longest */
    TEST_ASSERT_EQUAL(SLOTS, espos_httpd_auth_policy_sessions_live(&P));
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, a));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, b));
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, c));
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, d));
    /* an expired slot is reused before anyone live is evicted */
    fresh(10, false, KEY);
    login(a);
    F.now_s += 10;
    login(b);
    login(c);
    login(d);
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_valid(&P, a));
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, b));
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_session_valid(&P, d));
}

TEST_CASE("no session table: login fails, nothing else does", "[auth]")
{
    memset(&F, 0, sizeof(F));
    espos_httpd_auth_policy_init(&P, &PORT, NULL, NULL, 0, 3600, false);
    espos_httpd_auth_policy_set_key(&P, KEY);
    char sid[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_open(&P, sid, sizeof(sid)));
    TEST_ASSERT_EQUAL(0, espos_httpd_auth_policy_sessions_live(&P));
    espos_httpd_auth_request_t rq = { .bearer = KEY };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, NULL));
    /* and a too-small buffer is refused rather than overrun */
    fresh(3600, false, KEY);
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_session_open(&P, sid, ESPOS_HTTPD_AUTH_SID_LEN));
}

/* ------------------------------------------------------------------ origin */

TEST_CASE("cookie writes need Origin (or Referer) host == Host", "[auth]")
{
    fresh(3600, false, KEY);
    char sid[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    login(sid);
    espos_httpd_auth_method_t m;
    espos_httpd_auth_request_t rq = { .cookie_sid = sid, .host = "192.168.1.42" };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m)); /* a read */
    rq.state_changing = true;
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_FORBIDDEN_ORIGIN, decide(&rq, &m)); /* no Origin at all */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_COOKIE, m);                        /* it did authenticate */
    rq.origin = "http://evil.example";
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_FORBIDDEN_ORIGIN, decide(&rq, &m));
    rq.origin = "http://192.168.1.42";
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    rq.origin = "http://192.168.1.42/config#httpd"; /* a Referer */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    rq.host = NULL;
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_FORBIDDEN_ORIGIN, decide(&rq, &m));
}

TEST_CASE("origin matching: authority only, case-insensitive, default port dropped", "[auth]")
{
    TEST_ASSERT_TRUE(espos_httpd_auth_origin_matches("http://boat.local", "boat.local"));
    TEST_ASSERT_TRUE(espos_httpd_auth_origin_matches("HTTP://Boat.Local", "boat.local"));
    TEST_ASSERT_TRUE(espos_httpd_auth_origin_matches("http://boat.local:80", "boat.local"));
    TEST_ASSERT_TRUE(espos_httpd_auth_origin_matches("http://boat.local", "boat.local:80"));
    TEST_ASSERT_TRUE(espos_httpd_auth_origin_matches("http://10.0.0.2:8080", "10.0.0.2:8080"));
    TEST_ASSERT_TRUE(espos_httpd_auth_origin_matches("http://10.0.0.2:8080/wifi?x=1#y", "10.0.0.2:8080"));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("http://10.0.0.2:8080", "10.0.0.2"));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("http://10.0.0.2", "10.0.0.20"));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("http://10.0.0.20", "10.0.0.2"));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("null", "boat.local"));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("", "boat.local"));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches(NULL, "boat.local"));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("http://boat.local", NULL));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("http://boat.local", ""));
    TEST_ASSERT_FALSE(espos_httpd_auth_origin_matches("http://", "boat.local"));
}

/* ---------------------------------------------------------------- throttle */

TEST_CASE("throttle: five misses in the window lock every key check for the lockout", "[auth]")
{
    fresh(3600, false, KEY);
    for (int i = 0; i < ESPOS_HTTPD_AUTH_FAIL_MAX - 1; i++) {
        TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, espos_httpd_auth_policy_check_key(&P, "wrong"));
        TEST_ASSERT_FALSE(espos_httpd_auth_policy_throttled(&P));
        F.now_s += 10;
    }
    /* four misses over 40 s: still just misses */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, espos_httpd_auth_policy_check_key(&P, KEY));
    TEST_ASSERT_EQUAL(0, P.fail_count); /* a hit resets the count */
    for (int i = 0; i < ESPOS_HTTPD_AUTH_FAIL_MAX; i++) {
        TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, espos_httpd_auth_policy_check_key(&P, "wrong"));
    }
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_throttled(&P));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_LOCKOUT_S, espos_httpd_auth_policy_retry_after_s(&P));
    /* the right key is refused too while locked, and via decide as well */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_THROTTLED, espos_httpd_auth_policy_check_key(&P, KEY));
    espos_httpd_auth_method_t m;
    espos_httpd_auth_request_t rq = { .bearer = KEY };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_THROTTLED, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_NONE, m);
    /* a live cookie is not a key check */
    char sid[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    login(sid);
    espos_httpd_auth_request_t cookie = { .cookie_sid = sid };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&cookie, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_COOKIE, m);
    /* nothing without a credential is 429 either: it is 401 */
    espos_httpd_auth_request_t none = { 0 };
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, decide(&none, &m));
    F.now_s += ESPOS_HTTPD_AUTH_LOCKOUT_S - 1;
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_throttled(&P));
    TEST_ASSERT_EQUAL(1, espos_httpd_auth_policy_retry_after_s(&P));
    F.now_s += 1;
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_throttled(&P));
    TEST_ASSERT_EQUAL(0, espos_httpd_auth_policy_retry_after_s(&P));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_ALLOW, decide(&rq, &m));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_BEARER, m);
}

TEST_CASE("throttle: misses older than the window do not add up", "[auth]")
{
    fresh(3600, false, KEY);
    for (int i = 0; i < ESPOS_HTTPD_AUTH_FAIL_MAX - 1; i++) {
        TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, espos_httpd_auth_policy_check_key(&P, "wrong"));
    }
    F.now_s += ESPOS_HTTPD_AUTH_FAIL_WINDOW_S; /* the window is over */
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_UNAUTHORIZED, espos_httpd_auth_policy_check_key(&P, "wrong"));
    TEST_ASSERT_FALSE(espos_httpd_auth_policy_throttled(&P));
    TEST_ASSERT_EQUAL(1, P.fail_count);
    /* a new key does not end a lockout */
    for (int i = 0; i < ESPOS_HTTPD_AUTH_FAIL_MAX; i++) {
        espos_httpd_auth_policy_check_key(&P, "wrong");
    }
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_throttled(&P));
    espos_httpd_auth_policy_set_key(&P, "fresh-key-fresh-key");
    TEST_ASSERT_TRUE(espos_httpd_auth_policy_throttled(&P));
    TEST_ASSERT_EQUAL(ESPOS_HTTPD_AUTH_THROTTLED, espos_httpd_auth_policy_check_key(&P, "fresh-key-fresh-key"));
}

TEST_CASE("method names", "[auth]")
{
    TEST_ASSERT_EQUAL_STRING("none", espos_httpd_auth_method_str(ESPOS_HTTPD_AUTH_NONE));
    TEST_ASSERT_EQUAL_STRING("bearer", espos_httpd_auth_method_str(ESPOS_HTTPD_AUTH_BEARER));
    TEST_ASSERT_EQUAL_STRING("cookie", espos_httpd_auth_method_str(ESPOS_HTTPD_AUTH_COOKIE));
    TEST_ASSERT_EQUAL_STRING("portal", espos_httpd_auth_method_str(ESPOS_HTTPD_AUTH_PORTAL));
    TEST_ASSERT_EQUAL_STRING("none", espos_httpd_auth_method_str(ESPOS_HTTPD_AUTH_METHOD_MAX));
}
