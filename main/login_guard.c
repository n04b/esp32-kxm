#include "login_guard.h"

#include <string.h>
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "login_guard";

#define NVS_NS        "kvm"
#define NVS_KEY       "lockout"
#define STATE_VERSION 1

/* Lockout length per level; the last value repeats for every further round. */
static const uint32_t s_lock_s[] = { 60, 5 * 60, 15 * 60, 60 * 60 };
#define N_LEVELS ((uint8_t)(sizeof(s_lock_s) / sizeof(s_lock_s[0])))

typedef struct {
    uint8_t version;
    uint8_t fails;      /* wrong passwords in the current round */
    uint8_t level;      /* lockouts since the last correct password, capped at N_LEVELS */
    uint8_t locked;     /* a lockout is in progress */
} guard_state_t;

static guard_state_t s_st;
static int64_t s_until_us;  /* esp_timer time when the current lockout ends */

static uint32_t lock_len_s(uint8_t level)
{
    if (level == 0) level = 1;
    if (level > N_LEVELS) level = N_LEVELS;
    return s_lock_s[level - 1];
}

/* Called only when the state actually changes, to keep flash wear low. */
static void save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s_st, sizeof(s_st));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save lockout state: %s", esp_err_to_name(err));
}

void login_guard_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.version = STATE_VERSION;
    s_until_us = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        guard_state_t tmp;
        size_t len = sizeof(tmp);
        if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
            tmp.version == STATE_VERSION) {
            s_st = tmp;
            if (s_st.fails >= LOGIN_GUARD_FAILS_PER_ROUND) s_st.fails = 0;
            if (s_st.level > N_LEVELS) s_st.level = N_LEVELS;
        }
        nvs_close(h);
    }
    if (s_st.locked) {
        uint32_t len = lock_len_s(s_st.level);
        s_until_us = esp_timer_get_time() + (int64_t)len * 1000000;
        ESP_LOGW(TAG, "Login lockout was active before reboot, locked again for %lu s (level %u)",
                 (unsigned long)len, s_st.level);
    } else if (s_st.fails || s_st.level) {
        ESP_LOGI(TAG, "Failed logins: %u in this round, level %u", s_st.fails, s_st.level);
    }
}

uint32_t login_guard_wait_s(void)
{
    if (!s_st.locked) return 0;
    int64_t left = s_until_us - esp_timer_get_time();
    if (left > 0) return (uint32_t)((left + 999999) / 1000000);
    /* Lockout is over: clear the flag so a reboot does not lock again. */
    s_st.locked = 0;
    save();
    return 0;
}

uint32_t login_guard_fail(uint32_t client_ip)
{
    uint32_t started = 0;
    if (++s_st.fails >= LOGIN_GUARD_FAILS_PER_ROUND) {
        s_st.fails = 0;
        if (s_st.level < N_LEVELS) s_st.level++;
        s_st.locked = 1;
        started = lock_len_s(s_st.level);
        s_until_us = esp_timer_get_time() + (int64_t)started * 1000000;
        ESP_LOGW(TAG, "%d wrong passwords, last from %lu.%lu.%lu.%lu: login locked for %lu s (level %u)",
                 LOGIN_GUARD_FAILS_PER_ROUND,
                 (unsigned long)(client_ip >> 24), (unsigned long)((client_ip >> 16) & 0xFF),
                 (unsigned long)((client_ip >> 8) & 0xFF), (unsigned long)(client_ip & 0xFF),
                 (unsigned long)started, s_st.level);
    } else {
        ESP_LOGW(TAG, "Wrong password from %lu.%lu.%lu.%lu (%u of %d)",
                 (unsigned long)(client_ip >> 24), (unsigned long)((client_ip >> 16) & 0xFF),
                 (unsigned long)((client_ip >> 8) & 0xFF), (unsigned long)(client_ip & 0xFF),
                 s_st.fails, LOGIN_GUARD_FAILS_PER_ROUND);
    }
    save();
    return started;
}

uint32_t login_guard_attempts_left(void)
{
    return LOGIN_GUARD_FAILS_PER_ROUND - s_st.fails;
}

void login_guard_success(void)
{
    if (!s_st.fails && !s_st.level && !s_st.locked) return;
    s_st.fails = 0;
    s_st.level = 0;
    s_st.locked = 0;
    s_until_us = 0;
    save();
}
