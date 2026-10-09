#pragma once

#include <stddef.h>

typedef enum {
    VOICE_IOT_COMMAND_NOT_HANDLED = 0,
    VOICE_IOT_COMMAND_EXECUTED,
    VOICE_IOT_COMMAND_FAILED,
    /* The countdown is reserved now and started by the voice service only
     * after the cloud acknowledgement TTS drains or the chain exits. */
    VOICE_IOT_COMMAND_COUNTDOWN_PENDING,
    /* Radio play is acknowledged by the cloud reply; the radio stream is
     * started only after that reply drains, so the acknowledgement TTS cannot
     * interrupt the stream right after it begins. */
    VOICE_IOT_COMMAND_RADIO_PLAY,
    /* The utterance is a device command this service knows, but the cloud MCP
     * tool owns it, so the shortcut stands down.  The caller must treat it as
     * a command ("正在执行" confirmation) instead of falling through to the
     * chat/query prompts. */
    VOICE_IOT_COMMAND_DEFERRED,
    /* The radio was paused/stopped right here, so the station already changed
     * state before the cloud answered.  The caller must silence the cloud's own
     * radio tools for the rest of the turn: the speech model only ever saw the
     * (mangled) transcript, and given "电台。" it picks "play" - which used to
     * restart the station the local shortcut had just stopped. */
    VOICE_IOT_COMMAND_RADIO_HANDLED,
    /* "换一个台"类：不点名，直接切下一个频道。和 RADIO_PLAY 一样要延迟到云端
     * 应答播完之后才动电台——那段 TTS 占着共享播放通道，当场切过去只会被静音，
     * 用户听到的就是"换了但没声"。 */
    VOICE_IOT_COMMAND_RADIO_NEXT,
} voice_iot_command_result_t;

/* Executes only the small local-command allowlist (device control + timers).
 * The caller retains normal cloud conversation handling for every result.
 * When the command produces a local reply (timer query/acknowledgement),
 * announce receives the reply text (truncated to announce_size - 1); pass
 * NULL to discard it. */
voice_iot_command_result_t voice_iot_command_service_execute(
    const char *transcript, char *announce, size_t announce_size);
