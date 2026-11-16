#include "web_ui.h"
#include "app_config.h"
#include "kvm_input.h"
#include "kvm_rx.h"
#include "net_wifi.h"
#include "ts_client.h"
#include "usb_hid.h"
#include "login_guard.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "microlink.h"

static const char *TAG = "web_ui";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static httpd_handle_t s_hd;
static bool s_setup;

/* ── Sessions ───────────────────────────────────────────────────── */

#define MAX_SESSIONS 4
#define SESSION_TTL_US (24LL * 3600 * 1000000)
typedef struct { char token[33]; int64_t expires; } session_t;
static session_t s_sessions[MAX_SESSIONS];

/* ── WebSocket ──────────────────────────────────────────────────── */

#define MAX_WS 4
static int s_ws_fds[MAX_WS] = { -1, -1, -1, -1 };
static volatile int s_ws_owner = -1;       /* fd of the browser in control */

/* ── Utilities ──────────────────────────────────────────────────── */

static bool ip_is_tailnet(uint32_t ip) { return (ip & 0xFFC00000u) == 0x64400000u; }

static uint32_t sock_ip(int fd, bool local)
{
    struct sockaddr_storage ss;
    socklen_t l = sizeof(ss);
    int r = local ? getsockname(fd, (struct sockaddr *)&ss, &l)
                  : getpeername(fd, (struct sockaddr *)&ss, &l);
    if (r != 0) return 0;
    if (ss.ss_family == AF_INET) {
        return ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
    }
#if CONFIG_LWIP_IPV6
    if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
        uint32_t v;
        memcpy(&v, &a6->sin6_addr.s6_addr[12], 4);   /* IPv4-mapped */
        return ntohl(v);
    }
#endif
    return 0;
}

/* Whether UI access is allowed via this device address. */
static bool access_allowed(int fd)
{
    if (s_setup) return true;
    kvm_settings_t c;
    config_get(&c);
    uint32_t lip = sock_ip(fd, true);
    return ip_is_tailnet(lip) ? c.web_ts : c.web_lan;
}

static esp_err_t on_open(httpd_handle_t hd, int fd)
{
    (void)hd;
    if (!access_allowed(fd)) {
        ESP_LOGW(TAG, "UI access via this address is disabled");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    for (int i = 0; i < MAX_WS; i++) if (s_ws_fds[i] == fd) s_ws_fds[i] = -1;
    if (s_ws_owner == fd) {
        kvm_source_t src = { .kind = KVM_SRC_WS, .fd = fd };
        kvm_input_release_source(&src);
        s_ws_owner = -1;
        ESP_LOGI(TAG, "Browser disconnected, control released");
    }
    close(fd);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_cjson(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(req);
    esp_err_t r = send_json(req, s);
    free(s);
    return r;
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    char buf[200];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return send_json(req, buf);
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 4096) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static const char *jstr(cJSON *j, const char *k)
{
    cJSON *i = cJSON_GetObjectItem(j, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

static bool jbool(cJSON *j, const char *k, bool def)
{
    cJSON *i = cJSON_GetObjectItem(j, k);
    return cJSON_IsBool(i) ? cJSON_IsTrue(i) : def;
}

static int jint(cJSON *j, const char *k, int def)
{
    cJSON *i = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(i) ? i->valueint : def;
}

/* ── Authorization ──────────────────────────────────────────────── */

static bool get_cookie_token(httpd_req_t *req, char out[33])
{
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len > 900) return false;
    char *c = malloc(len + 1);
    if (!c) return false;
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", c, len + 1) == ESP_OK) {
        char *p = strstr(c, "kvm_sid=");
        if (p) {
            p += 8;
            int i = 0;
            while (i < 32 && p[i] && p[i] != ';') { out[i] = p[i]; i++; }
            out[i] = 0;
            ok = (i == 32);
        }
    }
    free(c);
    return ok;
}

static bool is_authed(httpd_req_t *req)
{
    if (s_setup) return true;        /* initial setup: no password yet */
    char tok[33];
    if (!get_cookie_token(req, tok)) return false;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] && s_sessions[i].expires > now &&
            strcmp(s_sessions[i].token, tok) == 0) {
            s_sessions[i].expires = now + SESSION_TTL_US;
            return true;
        }
    }
    return false;
}

#define REQUIRE_AUTH(req) do { if (!is_authed(req)) return send_err(req, "401 Unauthorized", "auth"); } while (0)

/* 429 with the lockout length: in Retry-After and as "retry_after" in the JSON. */
static esp_err_t send_locked(httpd_req_t *req, uint32_t secs)
{
    char hdr[12], buf[120];
    snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)secs);
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_hdr(req, "Retry-After", hdr);
    snprintf(buf, sizeof(buf),
             "{\"error\":\"Too many wrong passwords, try again in %lu s\",\"retry_after\":%lu}",
             (unsigned long)secs, (unsigned long)secs);
    return send_json(req, buf);
}

/* Wrong password: count it, slow the client down, and answer 403 or 429. */
static esp_err_t reject_password(httpd_req_t *req, const char *what)
{
    uint32_t locked = login_guard_fail(sock_ip(httpd_req_to_sockfd(req), false));
    vTaskDelay(pdMS_TO_TICKS(500));
    if (locked) return send_locked(req, locked);
    char msg[100];
    uint32_t left = login_guard_attempts_left();
    snprintf(msg, sizeof(msg), "%s, %lu attempt%s left before lockout",
             what, (unsigned long)left, left == 1 ? "" : "s");
    return send_err(req, "403 Forbidden", msg);
}

static void new_session(char out[33])
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", rnd[i]);
    int64_t now = esp_timer_get_time();
    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expires < s_sessions[slot].expires) slot = i;
        if (s_sessions[i].expires < now) { slot = i; break; }
    }
    strcpy(s_sessions[slot].token, out);
    s_sessions[slot].expires = now + SESSION_TTL_US;
}

/* ── Page ───────────────────────────────────────────────────────── */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                           index_html_gz_end - index_html_gz_start);
}

static esp_err_t h_state(httpd_req_t *req)
{
    char buf[220];
    snprintf(buf, sizeof(buf), "{\"mode\":\"%s\",\"authed\":%s,\"version\":\"%s\",\"ota\":%s,\"login_wait\":%lu}",
             s_setup ? "setup" : "run", is_authed(req) ? "true" : "false",
             esp_app_get_description()->version,
#if CONFIG_KVM_ENABLE_OTA
             "true",
#else
             "false",
#endif
             (unsigned long)(s_setup ? 0 : login_guard_wait_s())
    );
    return send_json(req, buf);
}

static esp_err_t h_login(httpd_req_t *req)
{
    uint32_t wait = login_guard_wait_s();
    if (wait) return send_locked(req, wait);
    cJSON *j = read_json(req);
    const char *pw = j ? jstr(j, "password") : NULL;
    bool ok = pw && config_check_password(pw);
    if (j) cJSON_Delete(j);
    if (!ok) return reject_password(req, "Wrong password");
    login_guard_success();
    char tok[33], cookie[100];
    new_session(tok);
    snprintf(cookie, sizeof(cookie), "kvm_sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400", tok);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_logout(httpd_req_t *req)
{
    char tok[33];
    if (get_cookie_token(req, tok)) {
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (strcmp(s_sessions[i].token, tok) == 0) memset(&s_sessions[i], 0, sizeof(session_t));
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "kvm_sid=; Path=/; Max-Age=0");
    return send_json(req, "{\"ok\":true}");
}

/* ── Status ─────────────────────────────────────────────────────── */

static const char *src_kind_str(kvm_src_kind_t k)
{
    switch (k) {
    case KVM_SRC_UDP_TS:  return "udp_tailnet";
    case KVM_SRC_UDP_LAN: return "udp_lan";
    case KVM_SRC_WS:      return "browser";
    default:              return "none";
    }
}

static esp_err_t h_status(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char ip[16];
    cJSON *j = cJSON_CreateObject();

    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddBoolToObject(w, "connected", net_wifi_connected());
    cJSON_AddStringToObject(w, "ssid", s_setup ? net_wifi_ap_ssid() : net_wifi_ssid());
    cJSON_AddNumberToObject(w, "rssi", net_wifi_rssi());
    microlink_ip_to_str(net_wifi_sta_ip(), ip);
    cJSON_AddStringToObject(w, "ip", ip);

    cJSON *t = cJSON_AddObjectToObject(j, "tailscale");
    cJSON_AddStringToObject(t, "state", ts_client_state_str());
    microlink_ip_to_str(ts_client_vpn_ip(), ip);
    cJSON_AddStringToObject(t, "ip", ip);
    const char *err = ts_client_error();
    if (err) cJSON_AddStringToObject(t, "error", err);

    kvm_stats_t st;
    kvm_input_get_stats(&st);
    cJSON *k = cJSON_AddObjectToObject(j, "kvm");
    cJSON_AddNumberToObject(k, "rx", st.rx_total);
    cJSON_AddNumberToObject(k, "dropped", st.dropped);
    cJSON_AddNumberToObject(k, "dropped_denied", st.dropped_denied);
    cJSON_AddNumberToObject(k, "dropped_busy", st.dropped_busy);
    cJSON_AddNumberToObject(k, "dropped_stale", st.dropped_stale);
    cJSON_AddNumberToObject(k, "dropped_bad", st.dropped_bad);
    cJSON_AddNumberToObject(k, "rate_hz", st.rate_hz);
    cJSON_AddNumberToObject(k, "wd_trips", st.wd_trips);
    cJSON_AddStringToObject(k, "source", src_kind_str(st.active.kind));
    if (st.active.kind == KVM_SRC_UDP_TS || st.active.kind == KVM_SRC_UDP_LAN) {
        microlink_ip_to_str(st.active.ip, ip);
        cJSON_AddStringToObject(k, "source_ip", ip);
    }
    if (st.active.kind == KVM_SRC_UDP_TS) {
        int d = ts_client_peer_direct(st.active.ip);
        cJSON_AddStringToObject(k, "path", d == 1 ? "direct" : d == 0 ? "derp" : "unknown");
    }

    cJSON *u = cJSON_AddObjectToObject(j, "usb");
    cJSON_AddBoolToObject(u, "mounted", usb_hid_mounted());
    cJSON_AddBoolToObject(u, "suspended", usb_hid_suspended());
    cJSON_AddBoolToObject(u, "boot_protocol", usb_hid_kbd_boot_protocol());
    cJSON_AddNumberToObject(u, "leds", usb_hid_leds());

    cJSON *m = cJSON_AddObjectToObject(j, "mem");
    cJSON_AddNumberToObject(m, "sram_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(m, "sram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(m, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(m, "psram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    return send_cjson(req, j);
}

static esp_err_t h_peers(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(req);
    ts_client_peers_json(buf, 2048);
    esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(req);
    if (net_wifi_scan_json(buf, 2048) != ESP_OK) strcpy(buf, "[]");
    esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

/* ── Settings ───────────────────────────────────────────────────── */

static esp_err_t h_config_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    kvm_settings_t c;
    config_get(&c);
    char ip[16];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid", c.wifi_ssid);
    cJSON_AddBoolToObject(j, "wifi_pass_set", c.wifi_pass[0] != 0);   /* secrets are never returned */
    cJSON_AddBoolToObject(j, "ts_auth_key_set", c.ts_auth_key[0] != 0);
    cJSON_AddStringToObject(j, "ts_ctrl_host", c.ts_ctrl_host);
    cJSON_AddStringToObject(j, "ts_hostname", c.ts_hostname);
    cJSON_AddNumberToObject(j, "udp_port", c.udp_port);
    cJSON_AddNumberToObject(j, "wd_ms", c.wd_ms);
    cJSON_AddNumberToObject(j, "udp_wd_ms", c.udp_wd_ms);
    cJSON_AddBoolToObject(j, "lan_rx", c.lan_rx);
    cJSON_AddBoolToObject(j, "web_lan", c.web_lan);
    cJSON_AddBoolToObject(j, "web_ts", c.web_ts);
    cJSON *a = cJSON_AddArrayToObject(j, "allow");
    for (int i = 0; i < c.allow_count; i++) {
        cJSON *e = cJSON_CreateObject();
        microlink_ip_to_str(c.allow_ip[i], ip);
        cJSON_AddStringToObject(e, "ip", ip);
        cJSON_AddStringToObject(e, "label", c.allow_label[i]);
        cJSON_AddItemToArray(a, e);
    }
    return send_cjson(req, j);
}

static bool copy_str(char *dst, size_t n, const char *src)
{
    if (!src) return false;
    if (strlen(src) >= n) return false;
    strcpy(dst, src);
    return true;
}

/* Apply JSON fields to the settings. Returns an error message or NULL. */
static const char *apply_fields(cJSON *j, kvm_settings_t *c, bool *need_restart)
{
    const char *s;
    if ((s = jstr(j, "wifi_ssid"))) {
        if (!s[0] || !copy_str(c->wifi_ssid, sizeof(c->wifi_ssid), s)) return "Invalid network name";
        *need_restart = true;
    }
    if ((s = jstr(j, "wifi_pass")) && s[0]) {
        if (strlen(s) < 8 || !copy_str(c->wifi_pass, sizeof(c->wifi_pass), s)) return "WiFi password: 8-64 characters";
        *need_restart = true;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "wifi_open"))) {
        c->wifi_pass[0] = 0;
        *need_restart = true;
    }
    if ((s = jstr(j, "ts_auth_key")) && s[0]) {
        if (!copy_str(c->ts_auth_key, sizeof(c->ts_auth_key), s)) return "Auth key is too long";
        *need_restart = true;
    }
    if ((s = jstr(j, "ts_ctrl_host"))) {
        if (!copy_str(c->ts_ctrl_host, sizeof(c->ts_ctrl_host), s)) return "Control server address is too long";
        *need_restart = true;
    }
    if ((s = jstr(j, "ts_hostname"))) {
        if (!copy_str(c->ts_hostname, sizeof(c->ts_hostname), s)) return "Device name is too long";
        for (const char *p = s; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
                  (*p >= 'A' && *p <= 'Z')))
                return "Device name: Latin letters, digits and hyphens";
        }
        *need_restart = true;
    }
    int v;
    if ((v = jint(j, "udp_port", -1)) != -1) {
        if (v < 1 || v > 65535) return "Invalid port";
        if (v != c->udp_port) *need_restart = true;
        c->udp_port = v;
    }
    if ((v = jint(j, "wd_ms", -1)) != -1) {
        if (v < 50 || v > 5000) return "Watchdog: 50-5000 ms";
        c->wd_ms = v;
    }
    if ((v = jint(j, "udp_wd_ms", -1)) != -1) {
        if (v != 0 && (v < 50 || v > 5000)) return "UDP watchdog: 0 or 50-5000 ms";
        c->udp_wd_ms = v;
    }
    c->lan_rx = jbool(j, "lan_rx", c->lan_rx);
    c->web_lan = jbool(j, "web_lan", c->web_lan);
    c->web_ts = jbool(j, "web_ts", c->web_ts);
    if (!c->web_lan && !c->web_ts) return "Cannot disable both web UI addresses";

    cJSON *a = cJSON_GetObjectItem(j, "allow");
    if (cJSON_IsArray(a)) {
        int n = cJSON_GetArraySize(a);
        if (n > KVM_MAX_ALLOWED) return "Too many addresses in the list";
        kvm_settings_t t = *c;
        t.allow_count = 0;
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(a, i);
            const char *ips = jstr(e, "ip");
            uint32_t ip = ips ? microlink_parse_ip(ips) : 0;
            if (!ip) return "Invalid IP address in the allowlist";
            t.allow_ip[t.allow_count] = ip;
            const char *lb = jstr(e, "label");
            strlcpy(t.allow_label[t.allow_count], lb ? lb : "", sizeof(t.allow_label[0]));
            t.allow_count++;
        }
        memcpy(c->allow_ip, t.allow_ip, sizeof(c->allow_ip));
        memcpy(c->allow_label, t.allow_label, sizeof(c->allow_label));
        c->allow_count = t.allow_count;
    }
    return NULL;
}

static void apply_runtime(const kvm_settings_t *c)
{
    kvm_input_set_watchdog(c->wd_ms, c->udp_wd_ms);
    kvm_rx_set_lan_enabled(c->lan_rx);
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (s_setup) return send_err(req, "409 Conflict", "Complete the initial setup first");
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Invalid JSON");
    kvm_settings_t c;
    config_get(&c);
    bool restart = false;
    const char *e = apply_fields(j, &c, &restart);
    cJSON_Delete(j);
    if (e) return send_err(req, "400 Bad Request", e);
    if (config_set(&c) != ESP_OK) return send_err(req, "500 Internal Server Error", "NVS write error");
    apply_runtime(&c);
    return send_json(req, restart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true}");
}

static void restart_later_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((int)(intptr_t)arg));
    esp_restart();
}

static void restart_later(int ms)
{
    xTaskCreate(restart_later_task, "restart", 2048, (void *)(intptr_t)ms, 1, NULL);
}

static esp_err_t h_setup(httpd_req_t *req)
{
    if (!s_setup) return send_err(req, "409 Conflict", "Device is already set up");
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Invalid JSON");
    kvm_settings_t c;
    config_get(&c);
    bool restart = false;
    const char *e = apply_fields(j, &c, &restart);
    const char *pw = jstr(j, "password");
    if (!e && (!c.wifi_ssid[0])) e = "Specify a WiFi network";
    if (!e && (!pw || config_hash_password(pw, &c) != ESP_OK)) e = "UI password: at least 6 characters";
    cJSON_Delete(j);
    if (e) return send_err(req, "400 Bad Request", e);
    c.provisioned = true;
    if (config_set(&c) != ESP_OK) return send_err(req, "500 Internal Server Error", "NVS write error");
    ESP_LOGI(TAG, "Initial setup complete, rebooting");
    restart_later(1500);
    return send_json(req, "{\"ok\":true,\"restart\":true}");
}

static esp_err_t h_password(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    uint32_t wait = login_guard_wait_s();
    if (wait) return send_locked(req, wait);
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Invalid JSON");
    const char *old = jstr(j, "old"), *nw = jstr(j, "new");
    esp_err_t r;
    if (!old || !config_check_password(old)) {
        r = reject_password(req, "Current password is wrong");
    } else {
        login_guard_success();
        kvm_settings_t c;
        config_get(&c);
        if (!nw || config_hash_password(nw, &c) != ESP_OK) {
            r = send_err(req, "400 Bad Request", "New password: at least 6 characters");
        } else {
            config_set(&c);
            memset(s_sessions, 0, sizeof(s_sessions));
            r = send_json(req, "{\"ok\":true}");
        }
    }
    cJSON_Delete(j);
    return r;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    restart_later(800);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_factory(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    config_factory_reset();
    restart_later(800);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_rereg(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    config_set_rereg_flag(true);
    restart_later(800);
    return send_json(req, "{\"ok\":true}");
}

#if CONFIG_KVM_ENABLE_OTA
static esp_err_t h_ota(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return send_err(req, "500 Internal Server Error", "No OTA partition");
    if (req->content_len == 0 || req->content_len > part->size)
        return send_err(req, "400 Bad Request", "Invalid firmware size");

    esp_ota_handle_t h;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
        return send_err(req, "500 Internal Server Error", "esp_ota_begin");
    kvm_input_release_all();

    char *buf = malloc(4096);
    size_t left = req->content_len;
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    while (err == ESP_OK && left > 0) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(h, buf, r);
        left -= r;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(h);
        return send_err(req, "500 Internal Server Error", "Error receiving firmware");
    }
    if (esp_ota_end(h) != ESP_OK)
        return send_err(req, "400 Bad Request", "Firmware image is corrupt or not for this board");
    if (esp_ota_set_boot_partition(part) != ESP_OK)
        return send_err(req, "500 Internal Server Error", "esp_ota_set_boot_partition");
    ESP_LOGI(TAG, "OTA written to %s, rebooting", part->label);
    restart_later(1000);
    return send_json(req, "{\"ok\":true}");
}
#endif

/* ── WebSocket ──────────────────────────────────────────────────── */

typedef struct { int fd; char msg[64]; } ws_msg_t;

static void ws_send_work(void *arg)
{
    ws_msg_t *m = arg;
    httpd_ws_frame_t f = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)m->msg,
        .len = strlen(m->msg),
        .final = true,
    };
    httpd_ws_send_frame_async(s_hd, m->fd, &f);
    free(m);
}

static void ws_send_text(int fd, const char *text)
{
    if (fd < 0 || !s_hd) return;
    ws_msg_t *m = malloc(sizeof(*m));
    if (!m) return;
    m->fd = fd;
    strlcpy(m->msg, text, sizeof(m->msg));
    if (httpd_queue_work(s_hd, ws_send_work, m) != ESP_OK) free(m);
}

static void send_hello(int fd)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"owner\":%s,\"leds\":%u}",
             s_ws_owner == fd ? "true" : "false", usb_hid_leds());
    ws_send_text(fd, msg);
}

static void on_leds(uint8_t leds)
{
    char msg[32];
    snprintf(msg, sizeof(msg), "{\"t\":\"leds\",\"v\":%u}", leds);
    int owner = s_ws_owner;
    if (owner >= 0) ws_send_text(owner, msg);
}

static esp_err_t h_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        if (!is_authed(req)) return ESP_FAIL;
        bool added = false;
        for (int i = 0; i < MAX_WS; i++) {
            if (s_ws_fds[i] == -1) { s_ws_fds[i] = fd; added = true; break; }
        }
        if (!added) return ESP_FAIL;
        if (s_ws_owner < 0) s_ws_owner = fd;
        send_hello(fd);
        return ESP_OK;
    }

    uint8_t buf[64];
    httpd_ws_frame_t f = { .payload = buf };
    if (httpd_ws_recv_frame(req, &f, sizeof(buf)) != ESP_OK) return ESP_FAIL;
    kvm_source_t src = { .kind = KVM_SRC_WS, .ip = sock_ip(fd, false), .fd = fd };

    if (f.type == HTTPD_WS_TYPE_BINARY) {
        if (s_ws_owner != fd) {
            static int64_t last_busy;
            int64_t now = esp_timer_get_time();
            if (now - last_busy > 1000000) { ws_send_text(fd, "{\"t\":\"busy\"}"); last_busy = now; }
            return ESP_OK;
        }
        if (kvm_input_packet(&src, buf, f.len) == KVM_IN_BUSY) {
            static int64_t last_src_busy;
            int64_t now = esp_timer_get_time();
            if (now - last_src_busy > 1000000) { ws_send_text(fd, "{\"t\":\"src_busy\"}"); last_src_busy = now; }
        }
    } else if (f.type == HTTPD_WS_TYPE_TEXT) {
        buf[f.len < sizeof(buf) ? f.len : sizeof(buf) - 1] = 0;
        if (strstr((char *)buf, "\"release\"")) {
            kvm_input_release_source(&src);
        } else if (strstr((char *)buf, "\"take\"")) {
            /* Explicit takeover of control by another browser. */
            int old = s_ws_owner;
            if (old != fd) {
                if (old >= 0) {
                    kvm_source_t o = { .kind = KVM_SRC_WS, .fd = old };
                    kvm_input_release_source(&o);
                    s_ws_owner = fd;
                    send_hello(old);
                } else {
                    s_ws_owner = fd;
                }
            }
            send_hello(fd);
        }
    }
    return ESP_OK;
}

/* ── Startup ────────────────────────────────────────────────────── */

esp_err_t web_ui_start(bool setup_mode)
{
    s_setup = setup_mode;
    login_guard_init();
    kvm_settings_t c;
    config_get(&c);
    apply_runtime(&c);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 24;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.core_id = 0;
    cfg.open_fn = on_open;
    cfg.close_fn = on_close;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_hd, &cfg);
    if (err != ESP_OK) return err;

    const httpd_uri_t uris[] = {
        { .uri = "/",                 .method = HTTP_GET,  .handler = h_index },
        { .uri = "/index.html",       .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/state",        .method = HTTP_GET,  .handler = h_state },
        { .uri = "/api/login",        .method = HTTP_POST, .handler = h_login },
        { .uri = "/api/logout",       .method = HTTP_POST, .handler = h_logout },
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/peers",        .method = HTTP_GET,  .handler = h_peers },
        { .uri = "/api/wifi/scan",    .method = HTTP_GET,  .handler = h_scan },
        { .uri = "/api/config",       .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/config",       .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/setup",        .method = HTTP_POST, .handler = h_setup },
        { .uri = "/api/password",     .method = HTTP_POST, .handler = h_password },
        { .uri = "/api/reboot",       .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/api/factory_reset",.method = HTTP_POST, .handler = h_factory },
        { .uri = "/api/ts/rereg",     .method = HTTP_POST, .handler = h_rereg },
#if CONFIG_KVM_ENABLE_OTA
        { .uri = "/api/ota",          .method = HTTP_POST, .handler = h_ota },
#endif
        { .uri = "/ws",               .method = HTTP_GET,  .handler = h_ws,
          .is_websocket = true, .handle_ws_control_frames = false },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_hd, &uris[i]);
    }
    usb_hid_set_leds_cb(on_leds);
    ESP_LOGI(TAG, "Web UI on port 80 (%s)", setup_mode ? "initial setup" : "normal mode");
    return ESP_OK;
}
