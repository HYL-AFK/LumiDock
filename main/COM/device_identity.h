/*
 * Device identity: the single name that both the Bluetooth advertisement and the
 * SoftAP SSID present to the outside world.
 *
 * Both names used to be independent literals -- the BLE side said "ESPARK-XXXXXX"
 * (three bytes of the Bluetooth MAC) while the AP said "ESP32S3-SETUP" (no MAC at
 * all). Phones in a shop with several units on the shelf could not tell them
 * apart, and even the suffixes would not have matched: ESP32-S3 hands out three
 * consecutive MACs (STA = base, AP = base + 1, BT = base + 2), so the Bluetooth
 * and WiFi suffixes would have differed in the last two hex digits if each side
 * had kept reading its own.
 *
 * Everything therefore derives from ESP_MAC_WIFI_STA, the base address, which is
 * also what device_data.c reports as the device MAC.
 */
#ifndef COM_DEVICE_IDENTITY_H
#define COM_DEVICE_IDENTITY_H

#include <stddef.h>

/* "ESP32S3_A1B2C3": prefix, underscore, then the low three bytes of the STA MAC. */
#define DEVICE_IDENTITY_PREFIX "ESP32S3"
#define DEVICE_IDENTITY_NAME_MAX 32

/*
 * The shared name, built once on first use and cached for the rest of the boot.
 *
 * Reading the MAC never touches the radio: esp_read_mac() comes from eFuse and
 * works before esp_wifi_init(), which matters because the Bluetooth stack asks
 * for this name during NimBLE sync and may run ahead of WiFi.
 *
 * If eFuse cannot be read the prefix is returned on its own rather than an empty
 * string -- a radio with no name is worse than a radio sharing one.
 */
const char *device_identity_name(void);

#endif /* COM_DEVICE_IDENTITY_H */
