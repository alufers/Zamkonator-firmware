#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "json.gen.h"

/**
 * @brief Start the event fan-out task.
 *
 * Must be called after config_init(). The fan-out task tolerates the SD card,
 * MQTT and the WebSocket not being up yet, so this can run early in app_main().
 */
void event_manager_init(void);

/**
 * @brief Submit a fully populated payload as an event.
 *
 * Fills in seq/uptime/time, marshals the envelope in the caller's context and
 * hands the JSON string to the fan-out task. Takes ownership of @p payload and
 * clears it before returning.
 *
 * Safe to call from any task (not from an ISR).
 */
void event_manager_submit(struct event_payload_t *payload);

/* ── Convenience emitters ────────────────────────────────────────────────── */

void event_emit_booted(void);
void event_emit_coredump_saved(const char *path, long size);
void event_emit_card_scanned(const struct ev_card_scanned_t *scan);
void event_emit_door_state(bool closed);
void event_emit_lock_state(bool locked);
void event_emit_push_to_exit(int input_idx);   /* 1-based slot number */
void event_emit_remote_open(const char *reason, int open_time_ms);

/* ── Consumers ───────────────────────────────────────────────────────────── */

/**
 * @brief Snapshot the in-memory ring as a JSON array, oldest event first.
 * Caller owns the returned sstr_t and must sstr_free() it.
 */
sstr_t event_manager_ring_json(void);

/** Event type name for a struct event_payload_t tag (used for MQTT topics). */
const char *event_manager_type_name(int payload_tag);
