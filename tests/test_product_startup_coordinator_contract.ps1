$ErrorActionPreference = 'Stop'

$coordinator = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\startup_coordinator.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\startup_coordinator.h"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$led = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\status_led_service.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"

@(
    @{ Content = $header; Pattern = 'startup_coordinator_init'; Message = 'Startup coordination needs a public initialization entry point.' },
    @{ Content = $coordinator; Pattern = 'STARTUP_SOUND_REMAINING_US \(4000000LL\)'; Message = 'The ready sound must be scheduled in the final four seconds of the boot animation.' },
    @{ Content = $coordinator; Pattern = 'ui_service_boot_is_active'; Message = 'Coordinator decisions must use the UI boot phase, not a duplicate fixed delay.' },
    @{ Content = $coordinator; Pattern = 'status_led_service_set_boot_animation_active\(true\)'; Message = 'The boot LED overlay must begin when boot animation begins.' },
    @{ Content = $coordinator; Pattern = 'status_led_service_set_boot_animation_active\(false\)'; Message = 'The boot LED overlay must release when boot animation completes.' },
    @{ Content = $coordinator; Pattern = 'startup_audio_play_ready'; Message = 'The final boot window must play the isolated startup PCM sound.' },
    @{ Content = $coordinator; Pattern = 'ui_service_note_boot_sound_finished'; Message = 'The coordinator must release UI transition only after local boot audio is finished or skipped.' },
    @{ Content = $coordinator; Pattern = 'xiaozhi_voice_service_start_boot_greeting'; Message = 'The coordinator must request the one-shot boot greeting only after voice readiness.' },
    @{ Content = $manager; Pattern = 'AUDIO_RETRY_DELAY_MS 3000U'; Message = 'ES8311 and voice startup failures must retry after three seconds.' },
    @{ Content = $manager; Pattern = 'early_audio_init_task'; Message = 'ES8311 must be initialized early only for the local boot sound.' },
    @{ Content = $manager; Pattern = 'while \(ui_service_boot_is_active\(\)\)'; Message = 'Early ES8311 retries must end with the boot phase and not overlap delayed voice startup.' },
    @{ Content = $manager; Pattern = 'startup_coordinator_init\(\)'; Message = 'Application startup must start the single startup coordinator.' },
    @{ Content = $ui; Pattern = 'UI_BOOT_MIN_DURATION_US \(10000000LL\)'; Message = 'Boot minimum duration must remain ten seconds.' },
    @{ Content = $ui; Pattern = 'UI_BOOT_MAX_DURATION_US \(15000000LL\)'; Message = 'Boot maximum duration must remain fifteen seconds.' },
    @{ Content = $ui; Pattern = 'UI_BOOT_SOUND_LEAD_US \(4000000LL\)'; Message = 'The UI must open the startup sound window four seconds before boot completion.' },
    @{ Content = $ui; Pattern = 'UI_BOOT_MAX_DURATION_US - UI_BOOT_SOUND_LEAD_US'; Message = 'Slow-network boot animation must still reserve its final four seconds for the startup sound.' },
    @{ Content = $ui; Pattern = 'elapsed_us >= UI_BOOT_MAX_DURATION_US && s_scene\.boot_sound_finished'; Message = 'Boot transition must not cut across a startup sound that began in the final window.' },
    @{ Content = $led; Pattern = 'STATUS_LED_BOOT_ANIMATION'; Message = 'The LED service needs a coordinator-owned boot animation state.' },
    @{ Content = $cmake; Pattern = 'APP/startup_coordinator\.c'; Message = 'Startup coordinator must be compiled.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Product startup coordinator contract passed.'
