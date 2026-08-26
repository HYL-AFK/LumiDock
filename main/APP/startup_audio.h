#pragma once

#include "esp_err.h"

/* Play the embedded local ready tone once.  This never enters the cloud TTS
 * queue and leaves the ES8311 DAC muted when it returns. */
esp_err_t startup_audio_play_ready(void);
