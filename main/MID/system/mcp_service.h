#pragma once

#include <stdbool.h>

#include "cJSON.h"

/* Handles one cloud MCP payload: a standard JSON-RPC 2.0 object that arrives
 * inside the websocket envelope
 *   {"session_id":"...","type":"mcp","payload":{...}}
 *
 * Returns the JSON-RPC object that has to be sent back, or NULL when the frame
 * is a notification (no id) or cannot be answered, in which case the caller
 * must stay silent.
 *
 * The returned pointer references an internal buffer that stays valid until the
 * next call, so the caller wraps it into the envelope right away:
 *   {"session_id":"...","type":"mcp","payload":<returned object>}
 *
 * Runs on the websocket session task and is not re-entrant. */
const char *mcp_service_handle(const cJSON *payload);

/* True once the cloud finished MCP discovery on the current connection.  From
 * that point the cloud can drive the volume itself, which is what lets the
 * local shortcut step aside instead of applying the change a second time. */
bool mcp_service_is_ready(void);

/* Clears the discovery state.  The voice service calls this whenever a new
 * websocket session starts, so a cloud that never speaks MCP keeps falling back
 * to the local commands. */
void mcp_service_reset(void);

/* Deferred starts raised by a tool call.
 *
 * A tool call always arrives before the cloud's acknowledgement TTS.  Starting
 * the radio stream right away would be cut off the moment that TTS opens the
 * shared playout, and the countdown would start ticking under the spoken
 * answer.  The tool handler only records the request; the voice service picks
 * it up with the same
 * "wait for the acknowledgement to drain" rule it uses for the local
 * commands and clears the flag through the matching clear call. */
bool mcp_service_pending_countdown(void);
void mcp_service_clear_pending_countdown(void);
bool mcp_service_pending_radio(void);
void mcp_service_clear_pending_radio(void);

/* Silences the cloud's own radio tools for the rest of the turn.
 *
 * The speech model never sees the audio, only the cloud transcript - and that
 * transcript is lossy: "关闭电台" arrived as "电台。" in the field log, so the
 * model picked self.radio.play and restarted the station the local shortcut
 * had just stopped.  Once a local radio command has been applied, this flag
 * makes every radio tool answer "true" without touching the radio service.
 * The voice service lifts it when the turn tears down. */
void mcp_service_set_radio_tools_blocked(bool blocked);
