# Emerald RainViewer Radar for ESP32-2432S08

A PlatformIO Arduino project that displays the latest RainViewer radar tile centered on Emerald, Queensland, on the 2.8-inch 320x240 TFT.

## Hardware assumptions

The project targets the commonly used ESP32-2432S028/Sunton-style board configuration: ILI9341 display, 320x240 pixels, and SPI pins MOSI=13, MISO=12, SCLK=14, CS=15, DC=2, with backlight on GPIO 21. `platformio.ini` configures those TFT_eSPI settings. If your ESP32-2432S08 revision has a different display driver or pinout, update the TFT_eSPI build flags and backlight pin to match its schematic.

## Configure Wi-Fi

Copy `include/secrets.h.example` to `include/secrets.h` and set your network name and password. `secrets.h` is intentionally not tracked by Git. Without it, the firmware compiles and displays a setup prompt.

## Build and upload

Install PlatformIO Core or the VS Code PlatformIO extension, connect the board, then run:

```sh
pio run
pio run --target upload
pio device monitor
```

The configured PlatformIO environment is `esp32dev`.

## What it does

- Connects to Wi-Fi and reads RainViewer's public weather-map metadata endpoint.
- Selects the newest `radar.past` frame and downloads the zoom-6 radar PNG tile containing Emerald (23.5273° S, 148.1635° E).
- Centers that tile on the screen, marks Emerald, and refreshes every 10 minutes.
- Uses HTTPS with certificate validation disabled for compatibility with the ESP32 Arduino TLS client; use certificate validation for deployments that require stronger transport verification.

RainViewer controls API availability, data coverage, and usage terms. The display is a single radar tile rather than a full street map; color and scale use RainViewer's tile defaults.

## Libraries

PlatformIO installs TFT_eSPI, PNGdec, and ArduinoJson from `platformio.ini`.
