#include "ble_provision_service.h"

#include "esp_log.h"

#include "ble_provision.h"
#include "mid_snapshot.h"

/* The final BluFi security and NVS profile handlers are kept behind this API.
 * BLE delivers credentials and the service publishes the provisioning state.
 * profile, then Wi-Fi reconnects. Hardware can be integrated without changing
 * APP or MID code when the final provisioning payload is fixed. */
esp_err_t ble_provision_service_init(void)
{
    bluetooth_snapshot_t snapshot = {
        .provisioned = ble_provision_is_ready(),
        .connected = ble_provision_is_connected(),
        .provisioning_active = ble_provision_is_window_active(),
    };
    mid_snapshot_set_bluetooth(&snapshot);
    ESP_LOGI("ble_provision", "BluFi provisioning service interface initialized");
    return ESP_OK;
}
