# LumiDock Software Foundation Verification

## Build

Target: ESP32-S3 with ESP-IDF 5.3.4.

Observed result:

- ESP-IDF build completed successfully.
- `build/LumiDock.bin` generated.
- Firmware size: `0x13f340` bytes.
- Factory partition: `0x200000` bytes; remaining space: `0xc0cd0` bytes.

## Implemented Paths

- `main/APP`: startup and main page rendering.
- `main/COM`: SW3518S, VoltPilot BMS, WiTime Wi-Fi/BluFi/time/weather adapters.
- `main/MID`: mutex-protected snapshots and event priority table.
- `main/Dri`: board pins, ST7789, radar, WS2812, I2S, button, MOS and I2C drivers.

## Hardware Assumptions

- SW3518S: 7-bit I2C address `0x3C`, SDA GPIO15, SCL GPIO16, 400 kHz.
- BMS: UART2, 9600 8N1, TX GPIO37, RX GPIO38, no DE/RE.
- BMS parser: VoltPilot field order with six status bytes, 14S/15S selection by frame length, and up to four revision-specific reserved bytes before CRC.
- Radar OUT: active high on GPIO44, following the WiTime presence model.
- MOS outputs start inactive; load mapping is intentionally deferred.
- Single-wire UART exposes an interface only; its protocol is deferred.

## Deferred Decisions

Button gestures, MOS load mapping, custom single-wire frames, MQTT versus HTTP, cloud versus offline ASR, and final battery detail fields remain outside this foundation build.
