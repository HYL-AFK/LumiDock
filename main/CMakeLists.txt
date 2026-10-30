idf_component_register(
    SRCS
        "main.c"
        "APP/app_manager.c"
        "APP/startup_coordinator.c"
        "APP/startup_audio.c"
        "APP/startup_gate.c"
        "APP/ui_service.c"
        "APP/ui_assets.c"
        "APP/ui_cjk_font.c"
        "APP/ui_cjk_font_radio.c"
        "APP/ui_cjk_font_34_black.c"
        "APP/ui_time_font.c"
        "MID/protocol/bms_protocol.c"
        "MID/protocol/bms_service.c"
        "MID/protocol/stc_protocol.c"
        "MID/protocol/stc_service.c"
        "MID/network/wifi_service.c"
        "MID/network/ble_provision_service.c"
        "MID/network/time_service.c"
        "MID/network/weather_service.c"
        "MID/network/tariff_service.c"
        "MID/network/news_service.c"
        "MID/network/http_memory_log.c"
        "MID/voice/xiaozhi_activation_service.c"
        "MID/voice/xiaozhi_voice_service.c"
        "MID/voice/xiaozhi_audio_pipeline.c"
        "Dri/st7789_driver.c"
        "Dri/ws2812_driver.c"
        "Dri/onboard_led_driver.c"
        "Dri/i2s_audio_driver.c"
        "Dri/button_driver.c"
        "Dri/ntc_sensor.c"
        "Dri/fan_control.c"
        "MID/mid_snapshot.c"
        "MID/system/watchdog_service.c"
        "MID/system/audio_playback_service.c"
        "MID/system/status_led_service.c"
        "MID/system/timer_service.c"
        "MID/system/timer_chime_audio.c"
        "MID/system/thermal_service.c"
        "MID/system/hourly_chime_service.c"
        "MID/system/voice_iot_command_service.c"
        "MID/system/mcp_service.c"
        "MID/system/device_settings_service.c"
        "MID/system/radio_stream_service.c"
        "MID/system/charge_strategy.c"
        "COM/app_config.c"
        "COM/device_data.c"
        "COM/location_snapshot.c"
        "COM/runtime_event_log.c"
        "COM/time_snapshot.c"
        "COM/weather_snapshot.c"
        "Dri/wifi_manager.c"
        "Dri/internet_health_policy.c"
        "MID/network/ble_provision.c"
        "MID/network/blufi_security.c"
        "APP/app_time.c"
        "APP/app_weather.c"
    EMBED_FILES
        "assets/ui/main_scene.rgb565"
        "assets/ui/boot_animation.gif"
        "assets/ui/wifi.rgb565"
        "assets/ui/bluetooth.rgb565"
        "assets/ui/battery.rgb565"
        "assets/ui/mic_error.rgb565"
        "assets/ui/timer_icon.rgb565a8"
        "assets/ui/alarm_icon.rgb565a8"
        "assets/ui/radio_next_icon.rgb565a8"
        "assets/ui/radio_next_left_icon.rgb565a8"
        "assets/ui/weather_icon_sunny.rgb565a8"
        "assets/ui/weather_icon_cloudy.rgb565a8"
        "assets/ui/weather_icon_rain.rgb565a8"
        "assets/ui/weather_icon_snow.rgb565a8"
        "assets/ui/weather_clear_scene.i8"
        "assets/ui/weather_cloudy_scene.i8"
        "assets/ui/weather_fog_scene.i8"
        "assets/ui/weather_rain_scene.i8"
        "assets/ui/weather_snow_scene.i8"
        "assets/ui/weather_thunder_scene.i8"
        "assets/ui/voice_connecting.i4"
        "assets/ui/voice_listening.i4"
        "assets/ui/voice_thinking.i4"
        "assets/ui/voice_speaking.i4"
        "assets/ui/voice_error.i4"
        "assets/audio/startup_ready_16k_mono_s16le.pcm"
        "assets/audio/error_tone_16k_mono_s16le.pcm"
        "assets/audio/prompt_executing_16k_mono_s16le.pcm"
        "assets/audio/prompt_querying_16k_mono_s16le.pcm"
        "assets/audio/prompt_thinking_16k_mono_s16le.pcm"
        # 本地问候片段（prompt_greet_*、prompt_hour_00~23、prompt_wx_*，共 38 个
        # 文件、约 1.45 MB）已从固件里摘除：开机问候改由云端 LLM 现场生成，
        # 见 xiaozhi_voice_service.c 的 begin_boot_greeting()。
        #
        # 源文件仍留在 main/assets/audio/ 下（约 1.45 MB 仓库体积），删掉它们不影响
        # 构建。留着是因为一旦云端问候出问题，用 generate_prompt_tones.py 把这几行
        # 加回来就能退回本地拼接方案。
        #
        # 例外：整点报时（hourly_chime_service.c）单独用了一套 07~21 共 15 个
        # 的 Opus 资产，合计约 103 KB。报时不适合走云端——整点那一刻断网就彻底
        # 不响，而本地素材是唯一与网络无关的做法。00~06 和 22~23 不在报时时段。
        #
        # 用 Opus 而不是 PCM，是因为报时语速要放慢到 -33%：同一句话裸 PCM 要
        # 78 KB，15 条就是 1.12 MB，而 Opus 只要 5.5 KB 一条。资产格式为
        # [u16 包数][u16 x N 包长][包数据连续存放]，由 generate_prompt_tones.py
        # 生成，设备端逐包解码后入队。
        "assets/audio/prompt_chime_hour_07_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_08_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_09_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_10_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_11_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_12_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_13_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_14_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_15_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_16_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_17_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_18_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_19_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_20_16k_mono_opus.bin"
        "assets/audio/prompt_chime_hour_21_16k_mono_opus.bin"
        "assets/audio/prompt_exit_16k_mono_s16le.pcm"
        "assets/audio/prompt_timeout_16k_mono_s16le.pcm"
        "assets/audio/prompt_fallback_16k_mono_s16le.pcm"
    INCLUDE_DIRS
        "."
        "APP"
        "COM"
        "Dri"
        "MID"
        "MID/network"
        "MID/protocol"
        "MID/system"
        "MID/voice"
    REQUIRES
        bt
        driver
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_pcnt
        esp_driver_spi
        esp_rom
        esp_app_format
        esp_event
        esp_http_client
        esp_netif
        esp_psram
        esp_system
        esp_timer
        esp_wifi
        esp_codec_dev
        esp_audio_codec
        esp_websocket_client
        esp-sr
        json
        lvgl
        mbedtls
        nvs_flash
)

# lwIP 的 DNS 等待表默认只有 4 个槽（lwip/src/include/lwip/opt.h 里
# "DNS_TABLE_SIZE 4"）。dns_enqueue() 在四个槽全被占着时直接 return ERR_MEM，
# 上层 getaddrinfo 拿到的就是 202——注意它是当场返回的，不是超时：实测两次
# internet probe 相隔 2 秒，两次都在打日志的同一毫秒里报 202。
#
# 这台设备同时活着五个域名：电台 ngcdn001.cnr.cn、语音 api.tenclass.net、
# 天气 api.open-meteo.com、定位 ipapi.co / ipwho.is、probe www.qq.com。
# 只要一瞬间有五个查询在飞，第五个就一定失败，而失败又正好落在 probe 上，
# 两次硬失败就把 STA 强制重连了，电台和语音一起断。实测一次 420 秒的抓取里
# 这样的假死有 7 次。加到 8 给并发留出余量；每个 entry 不到 100 字节的 .bss，
# 代价可以忽略。
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PUBLIC DNS_TABLE_SIZE=8)
