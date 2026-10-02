#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Outcome of a card lookup.
 *
 * The DENIED_* results are verdicts about the card and are terminal: the
 * online strategy answered, and the answer was "no". Only AUTH_FAIL_RETRYABLE
 * — the proxy being unhealthy, timing out, or returning 5xx — falls through to
 * the cached strategy. */
typedef enum {
    AUTH_GRANTED,
    AUTH_DENIED_NOT_FOUND,
    AUTH_DENIED_EXPIRED,
    AUTH_FAIL_RETRYABLE,
    AUTH_FAIL_UNCONFIGURED,   /* no auth_proxy_base_url set */
} auth_result_t;

/* Username is capped at 24 bytes on the wire (see the cache record layout),
 * so 25 bytes always holds it plus a terminator. */
typedef struct {
    char    username[25];
    int64_t membership_expiration;   /* 0 when unknown */
} auth_user_t;

/* Start the cached-user-database downloader. Call after sdcard_init(). */
void auth_init(void);

/**
 * @brief Resolve a card to a user.
 *
 * Tries auth_proxy_online first, falling back to auth_proxy_cached when the
 * online strategy fails in a retryable way and the cache is enabled.
 *
 * @param[out] out           Filled in on AUTH_GRANTED and AUTH_DENIED_EXPIRED.
 * @param[out] strategy_out  auth_strategy_name_t of the strategy that answered;
 *                           untouched when nothing answered.
 */
auth_result_t auth_check_card(uint32_t mifare_id, auth_user_t *out,
                              int *strategy_out);

/* Wake the downloader immediately — called when the proxy becomes healthy. */
void auth_cache_trigger_download(void);
