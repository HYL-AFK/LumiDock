#include "voice_iot_command_service.h"

#include <string.h>

#include "esp_err.h"

#include "status_led_service.h"
#include "ui_service.h"

static bool contains_any(const char *text, const char *const *phrases, size_t phrase_count)
{
    if (text == NULL) return false;
    for (size_t index = 0; index < phrase_count; ++index) {
        if (strstr(text, phrases[index]) != NULL) return true;
    }
    return false;
}

voice_iot_command_result_t voice_iot_command_service_execute(const char *transcript)
{
    static const char *const close_led_phrases[] = {"关闭灯带", "关灯带"};
    static const char *const open_led_phrases[] = {"打开灯带", "开灯带"};
    static const char *const exit_standby_phrases[] = {"退出待机", "唤醒屏幕"};

    if (contains_any(transcript, close_led_phrases,
                     sizeof(close_led_phrases) / sizeof(close_led_phrases[0]))) {
        return status_led_service_set_user_enabled(false) == ESP_OK ?
                   VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }
    if (contains_any(transcript, open_led_phrases,
                     sizeof(open_led_phrases) / sizeof(open_led_phrases[0]))) {
        return status_led_service_set_user_enabled(true) == ESP_OK ?
                   VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }
    if (contains_any(transcript, exit_standby_phrases,
                     sizeof(exit_standby_phrases) / sizeof(exit_standby_phrases[0]))) {
        return ui_service_request_exit_standby() == ESP_OK ?
                   VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }
    return VOICE_IOT_COMMAND_NOT_HANDLED;
}
