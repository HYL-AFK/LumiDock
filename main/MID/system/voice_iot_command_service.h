#pragma once

typedef enum {
    VOICE_IOT_COMMAND_NOT_HANDLED = 0,
    VOICE_IOT_COMMAND_EXECUTED,
    VOICE_IOT_COMMAND_FAILED,
} voice_iot_command_result_t;

/* Executes only the small local-command allowlist. The caller retains normal
 * cloud conversation handling for every result. */
voice_iot_command_result_t voice_iot_command_service_execute(const char *transcript);
