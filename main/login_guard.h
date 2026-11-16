/*
 * Brute-force protection for the web UI password.
 *
 * Every 5 consecutive wrong passwords lock password checks for a growing
 * delay: 1 min, 5 min, 15 min, then 1 h for every further round. A correct
 * password resets everything. The failure counter, lockout level and "locked"
 * flag live in NVS (namespace "kvm", key "lockout"), so a reboot does not reset
 * the protection: if the board restarts during a lockout, the delay of the
 * current level starts over (there is no real-time clock before SNTP).
 *
 * The counter is global, not per client IP. Not thread-safe: call only from
 * the HTTP server task.
 */
#pragma once

#include <stdint.h>

#define LOGIN_GUARD_FAILS_PER_ROUND 5

/* Load the state from NVS; restarts the lockout if one was active. */
void login_guard_init(void);

/* Seconds left in the current lockout, 0 if password checks are allowed. */
uint32_t login_guard_wait_s(void);

/* Record a wrong password. Returns the lockout in seconds that it started,
 * or 0 if no lockout started. client_ip (host byte order, 0 if unknown) is
 * only used for the log. */
uint32_t login_guard_fail(uint32_t client_ip);

/* Wrong passwords left before the next lockout. */
uint32_t login_guard_attempts_left(void);

/* Record a correct password: resets the counter and the level. */
void login_guard_success(void);
