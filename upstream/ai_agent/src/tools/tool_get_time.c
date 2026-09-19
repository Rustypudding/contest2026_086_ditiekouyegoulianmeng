/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "tools/tool_get_time.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "infra/http_proxy.h"
#include "infra/vela_tls.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

static const char *TAG = "tool_time";

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/* Set only when something authoritative has actually written the clock.
 *
 * This board has no RTC, so at boot the clock holds a seeded or placeholder
 * value that an magnitude check would happily accept. "Looks plausible" is
 * therefore NOT evidence that the clock is right — only a successful
 * settimeofday() from a network Date header is. Everything that schedules
 * a future absolute action must consult this rather than time(NULL). */
static volatile bool s_clock_verified = false;

bool agent_clock_is_verified(void)
{
    return s_clock_verified;
}

/* Parse "Sat, 01 Feb 2025 10:25:00 GMT" → set system clock, return formatted string */
static bool parse_and_set_time(const char *date_str, char *out, size_t out_size)
{
    int day, year, hour, min, sec;
    char mon_str[4] = {0};

    if (sscanf(date_str, "%*[^,], %d %3s %d %d:%d:%d",
               &day, mon_str, &year, &hour, &min, &sec) != 6) {
        return false;
    }

    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon_str, MONTHS[i]) == 0) { mon = i; break; }
    }
    if (mon < 0) return false;

    struct tm tm_utc;
    memset(&tm_utc, 0, sizeof(tm_utc));
    tm_utc.tm_sec  = sec;
    tm_utc.tm_min  = min;
    tm_utc.tm_hour = hour;
    tm_utc.tm_mday = day;
    tm_utc.tm_mon  = mon;
    tm_utc.tm_year = year - 1900;

    /* Convert UTC struct tm to UTC epoch.
     * mktime() interprets its argument as local time (per TZ), so we
     * must use timegm() or equivalent to get the correct UTC epoch.
     * NuttX provides timegm(); if unavailable, temporarily override TZ. */
#ifdef __NuttX__
    time_t t = timegm(&tm_utc);
#else
    /* Portable fallback: save TZ, set to UTC, call mktime, restore */
    char *old_tz = getenv("TZ");
    char  saved_tz[64] = {0};
    if (old_tz) strncpy(saved_tz, old_tz, sizeof(saved_tz) - 1);
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm_utc);
    if (old_tz) setenv("TZ", saved_tz, 1);
    else        unsetenv("TZ");
    tzset();
#endif
    if (t < 0) return false;

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) < 0) {
        syslog(LOG_ERR, "[%s] settimeofday failed: %d\n", TAG, errno);
        return false;
    }

    /* The clock now holds a time we got from a network peer, so it is
     * finally safe to schedule against. */
    s_clock_verified = true;

    /* Format in local time (TZ=CST-8 → UTC+8) using gmtime to avoid
     * zoneinfo lookup errors on NuttX romfs */
    struct tm local;
    time_t local_epoch = t + 8 * 3600;
    gmtime_r(&local_epoch, &local);

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S (%A)", &local);
    snprintf(out, out_size, "%s CST (UTC+8), UNIX epoch: %lld", time_str, (long long)t);

    return true;
}

/* ── Proxy path ─────────────────────────────────────────────── */

static int fetch_time_via_proxy(char *out, size_t out_size)
{
    proxy_conn_t *conn = proxy_conn_open("www.baidu.com", 443, 10000);
    if (!conn) return ERROR;

    const char *req =
        "HEAD / HTTP/1.1\r\n"
        "Host: www.baidu.com\r\n"
        "Connection: close\r\n\r\n";

    if (proxy_conn_write(conn, req, strlen(req)) < 0) {
        proxy_conn_close(conn);
        return ERROR;
    }

    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = proxy_conn_read(conn, buf + total, sizeof(buf) - 1 - total, 10000);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    proxy_conn_close(conn);

    char *date_hdr = strcasestr(buf, "\r\nDate: ");
    if (!date_hdr) return ERROR;
    date_hdr += 8; /* skip \r\nDate:  */

    char *eol = strstr(date_hdr, "\r\n");
    if (!eol) return ERROR;

    char date_val[64];
    size_t dlen = (size_t)(eol - date_hdr);
    if (dlen >= sizeof(date_val)) return ERROR;
    memcpy(date_val, date_hdr, dlen);
    date_val[dlen] = '\0';

    if (!parse_and_set_time(date_val, out, out_size)) return ERROR;
    return OK;
}

/* ── Direct path ──────────────────────────────────────────────── */

static int fetch_time_direct(char *out, size_t out_size)
{
    char date_val[64] = {0};
    int ret = vela_https_head_date("www.baidu.com", "443", "/", date_val, sizeof(date_val));
    if (ret != 0 || date_val[0] == '\0') return ERROR;
    if (!parse_and_set_time(date_val, out, out_size)) return ERROR;
    return OK;
}

/* ── Public sync entry point ──────────────────────────────────── */

int agent_time_sync_now(char *out, size_t out_size)
{
    /* Prefer an authoritative network sync. The on-board RTC can be stale
     * (no coin-cell, or the system NTP daemon not running), so trusting a
     * merely "plausible" local clock produced a ~30-day error. Fetch the
     * HTTPS Date header (it also settimeofday()s the clock). */
    char scratch[96];
    int err;

    if (!out) {
        out = scratch;
        out_size = sizeof(scratch);
    }

    if (http_proxy_is_enabled()) {
        err = fetch_time_via_proxy(out, out_size);
    } else {
        err = fetch_time_direct(out, out_size);
    }

    return (err == OK) ? OK : ERROR;
}

/* ── Entry point ──────────────────────────────────────────────── */

int tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    syslog(LOG_INFO, "[%s] Fetching current time...\n", TAG);

    if (agent_time_sync_now(output, output_size) == OK) {
        syslog(LOG_INFO, "[%s] Time (network): %s\n", TAG, output);
        return OK;
    }

    /* Network unavailable, so all that is left is the local clock — and it
     * is only usable if something authoritative has already set it. Do NOT
     * fall back on "the timestamp looks plausible": with no RTC the boot
     * seed and vela_tls's placeholder both pass any magnitude check while
     * being days off, and returning one of those as the current time is how
     * a reminder ends up scheduled against the wrong date. */
    if (agent_clock_is_verified()) {
        time_t now = time(NULL);
        struct tm utc_tm;
        time_t local_epoch = now + 8 * 3600;  /* UTC+8 */
        gmtime_r(&local_epoch, &utc_tm);

        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &utc_tm);
        snprintf(output, output_size,
                 "%s CST (UTC+8), UNIX epoch: %lld",
                 time_str, (long long)now);
        syslog(LOG_WARNING, "[%s] Time (local clock, synced earlier): %s\n", TAG, output);
        return OK;
    }

    snprintf(output, output_size,
             "Error: cannot determine the current time — no network time "
             "source, and the device clock has never been synced.");
    syslog(LOG_ERR, "[%s] %s\n", TAG, output);
    return ERROR;
}
