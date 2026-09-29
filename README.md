# ESP32 Air Quality Monitor

Firmware for a CO₂ / temperature / humidity monitor on **ESP32-S3** with a
capacitive touchscreen UI built on **LVGL 9** and **FreeRTOS**. Ships with
**host-based unit tests** so business logic is verified on every commit,
without a device.

## Features

- **Real-time sensing** — CO₂ (ppm), temperature (°C), humidity (%) from
  Sensirion SCD4X over I²C.
- **Touch UI** — 800×480 RGB LCD with LVGL 9. Tileview layout: dashboard
  page with animated arcs and a WiFi settings page.
- **WiFi service** — STA mode, auto-connect to saved networks, reconnect
  logic, credentials stored in NVS. Async API wrapped in FreeRTOS tasks.
- **Time sync** — SNTP with multiple fallback servers, applied at boot
  after IP is obtained.
- **Robust state handling** — explicit "target vs actual" SSID, atomic
  flags for cross-task state, mutex-protected scan results.
- **Host-based unit tests** — Unity + mocked ESP-IDF APIs. Runs on Linux
  in < 1 second.

## Hardware

| Component | Details |
|-----------|---------|
| MCU       | ESP32-S3 (dual-core, 240 MHz, 8 MB PSRAM) |
| Display   | 800×480 RGB LCD (Waveshare ESP32-S3-Touch-LCD-4.3) |
| Touch     | GT911 capacitive, I²C |
| Sensor    | Sensirion SCD41 (CO₂ / temp / humidity), I²C |

## Stack

- **Framework:** ESP-IDF 5.x
- **RTOS:** FreeRTOS
- **GUI:** LVGL 9 (via `esp_lvgl_port`)
- **Storage:** NVS (`wifi_db` namespace, CRC32-hashed SSID keys)
- **Tests:** Unity (host build, POSIX + mocked ESP-IDF)