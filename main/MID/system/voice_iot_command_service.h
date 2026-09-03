#pragma once

#include <stddef.h>

typedef enum {
    VOICE_IOT_COMMAND_NOT_HANDLED = 0,
    VOICE_IOT_COMMAND_EXECUTED,
    VOICE_IOT_COMMAND_FAILED,
    /* The countdown is reserved now and started by the voice service only
     * after the cloud acknowledgement TTS drains or the chain exits. */
    VOICE_IOT_COMMAND_COUNTDOWN_PENDING,
    /* Radio play is acknowledged through a cloud [device_call] reply; the
     * radio stream is started only after that reply finishes playing, so the
     * acknowledgement TTS cannot interrupt the stream right after it begins. */
    VOICE_IOT_COMMAND_RADIO_PLAY,
} voice_iot_command_result_t;

/* Executes only the small local-command allowlist (device control + timers).
 * The caller retains normal cloud conversation handling for every result.
 * When the command produces a local reply (timer query/acknowledgement),
 * announce receives the reply text (truncated to announce_size - 1); pass
 * NULL to discard it. */
voice_iot_command_result_t voice_iot_command_service_execute(
    const char *transcript, char *announce, size_t announce_size);
