# LumiDock Software Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended) to implement this plan task-by-task.

**Goal:** Build a modular ESP-IDF foundation for LumiDock with startup/main UI state, WiTime connectivity/time/weather reuse, VoltPilot BMS reuse, SW3518S I2C polling, and the confirmed board pin map.

**Architecture:** `APP` owns boot and UI state, `MID` owns cached snapshots and event priority, `COM` owns WiTime/VoltPilot protocol adapters, and `Dri` owns GPIO/UART/I2C/SPI hardware. The first build is read-only for SW3518S and BMS; deferred protocols expose safe no-op interfaces.

**Tech Stack:** ESP-IDF C, FreeRTOS, ESP-NETIF/Wi-Fi/BluFi, UART, I2C, SPI, existing WiTime and VoltPilot parsing logic.

---

### Task 1: Board and layer skeleton

**Files:**
- Create: `main/APP/app_manager.c`, `main/APP/app_manager.h`
- Create: `main/COM/com_status.h`, `main/MID/mid_snapshot.h`, `main/Dri/board_pins.h`
- Modify: `main/main.c`, `main/CMakeLists.txt`

- [ ] Define all confirmed GPIOs in `Dri/board_pins.h`, including ST7789, I2C, BMS RS485, radar, WS2812, button, MOS, and deferred single-wire UART.
- [ ] Define `lumidock_snapshot_t` with time, weather, Wi-Fi, Bluetooth, charging protocol, and BMS fields; protect access with a FreeRTOS mutex.
- [ ] Make `main/main.c` define `app_main()`, initialize the snapshot, and start the manager task.
- [ ] Register all four layer directories as component include paths and compile every source file in the component.

### Task 2: SW3518S read-only I2C driver and charging service

**Files:**
- Create: `main/Dri/sw3518s_i2c.c`, `main/Dri/sw3518s_i2c.h`
- Create: `main/COM/charging_service.c`, `main/COM/charging_service.h`
- Modify: `main/MID/mid_snapshot.h`, `main/APP/app_manager.c`

- [ ] Initialize I2C at 400 kHz with GPIO15 SDA, GPIO16 SCL, and 7-bit address `0x3C`.
- [ ] Implement register reads for `0x01`, `0x06`, `0x07`, `0x08`, `0x30`, `0x31`, `0x33`, `0x34`, and high-resolution ADC registers `0x3A`-`0x3C`.
- [ ] Decode protocol values including PD2.0/3.0, QC2.0/3.0, FCP, SCP, PD Fixed/PPS, PE, VOOC, SFCP, and AFC.
- [ ] Poll every 1000 ms, update a cached `charging_snapshot_t`, and never write SW3518S configuration or reset registers.
- [ ] Mark the snapshot invalid after a failed read and keep the last error for UI diagnostics.

### Task 3: VoltPilot BMS communication adapter

**Files:**
- Create: `main/COM/bms_protocol.c`, `main/COM/bms_protocol.h`
- Create: `main/COM/bms_service.c`, `main/COM/bms_service.h`
- Modify: `main/MID/mid_snapshot.h`, `main/APP/app_manager.c`

- [ ] Port the VoltPilot BMS request format, CRC16 validation, frame-length detection, and partial-frame buffer logic into LumiDock-owned files.
- [ ] Configure UART2 at 9600 8N1 using the board's BMS RX/TX pins and no DE/RE GPIO.
- [ ] Send one request per second, parse valid frames, and mark the BMS offline after 5 seconds without a valid response.
- [ ] Publish only the stable BMS snapshot fields required by the UI foundation: online, pack voltage, current, SOC, SOH, charge/discharge MOS state, and protection bytes.

### Task 4: WiTime reuse adapters

**Files:**
- Create: `main/COM/wifi_service.c`, `main/COM/wifi_service.h`
- Create: `main/COM/ble_provision_service.c`, `main/COM/ble_provision_service.h`
- Create: `main/COM/time_service.c`, `main/COM/time_service.h`
- Create: `main/COM/weather_service.c`, `main/COM/weather_service.h`
- Create: `main/Dri/radar_driver.c`, `main/Dri/radar_driver.h`
- Modify: `main/MID/mid_snapshot.h`, `main/APP/app_manager.c`

- [ ] Copy the WiTime APSTA Wi-Fi manager and standard BluFi provisioning implementation into LumiDock-owned COM files, preserving its NVS profile behavior.
- [ ] Copy the SNTP/timezone/location task interface and weather snapshot cache, with the existing WiTime network callbacks connected to the LumiDock snapshot.
- [ ] Port the WiTime radar parser and set its GPIO/baud/output-level constants from the WiTime implementation; expose only presence and distance to MID.
- [ ] Keep weather provider and cloud transport behind the existing service interface so MQTT/HTTP remains a later configuration choice.

### Task 5: UI drivers and pages

**Files:**
- Create: `main/Dri/st7789_driver.c`, `main/Dri/st7789_driver.h`
- Create: `main/Dri/ws2812_driver.c`, `main/Dri/ws2812_driver.h`
- Create: `main/Dri/i2s_audio_driver.c`, `main/Dri/i2s_audio_driver.h`
- Create: `main/Dri/button_driver.c`, `main/Dri/button_driver.h`
- Create: `main/APP/ui_service.c`, `main/APP/ui_service.h`

- [ ] Add a display-independent UI renderer interface with startup and main page states.
- [ ] Render startup name/self-test status, then main page time/timezone, weather/temperature, Wi-Fi, Bluetooth, and charging protocol from the MID snapshot.
- [ ] Use placeholders such as `--` for offline or not-yet-configured values; do not invent battery fields that the user has deferred.
- [ ] Debounce the single button and emit short/long/double events to MID without assigning final business actions.
- [ ] Initialize WS2812, I2S, and ST7789 with safe inactive outputs; leave audio behavior and MOS load behavior behind interfaces.

### Task 6: Event priority and deferred interfaces

**Files:**
- Create: `main/MID/event_bus.c`, `main/MID/event_bus.h`
- Create: `main/COM/single_wire_uart_service.c`, `main/COM/single_wire_uart_service.h`
- Create: `main/Dri/mos_driver.c`, `main/Dri/mos_driver.h`

- [ ] Implement priority ordering: protection/fault, system control, local button, radar, charging/external communication, cloud, ASR, and UI refresh.
- [ ] Make deferred single-wire UART functions return `ESP_ERR_NOT_SUPPORTED` until its protocol is defined.
- [ ] Initialize MOS outputs to their inactive level and expose enable/disable functions without selecting a load policy.

### Task 7: Build and verification

**Files:**
- Modify: `README.md`
- Create: `docs/superpowers/verification/2026-07-31-lumidock-software-foundation.md`

- [ ] Run `idf.py reconfigure` and `idf.py build` with the ESP32-S3 target.
- [ ] Run a source-level check that every declared header has one implementation and that no deferred service writes hardware.
- [ ] Verify the final GPIO table against `参考资料/LumiDock-已用引脚表-无颜色.docx` and record any board-specific assumptions.
- [ ] Document the exact build command, expected serial logs, SW3518S address, and the deferred decisions.
