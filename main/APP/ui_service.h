#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ui_service_init(void);
/* True after startup has reached a stable visual handoff; the network page
 * also opens this gate so deferred audio/WSS initialization can run while it
 * performs the connection decision. */
bool ui_service_is_main_stable(void);
/* Boot phase queries are consumed by the startup coordinator.  They expose
 * UI-owned timing so peripheral services do not recreate boot delays. */
bool ui_service_boot_is_active(void);
bool ui_service_boot_sound_window_open(void);
/* Called after the local boot PCM has completed or was skipped.  The UI will
 * not leave the animation page for a partially played sound. */
void ui_service_note_boot_sound_finished(void);
bool ui_service_boot_transition_complete(void);
/* True only after the post-boot network decision reached a healthy XiaoZhi WSS. */
bool ui_service_boot_cloud_ready(void);
/* Called by button/application code; the UI task consumes it on its next poll. */
void ui_service_note_activity(void);
/* Task-safe request to leave the dimmed standby page on the next UI poll. */
esp_err_t ui_service_request_exit_standby(void);
