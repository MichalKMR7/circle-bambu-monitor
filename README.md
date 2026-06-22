# Circle Bambu Monitor

Circle Bambu Monitor is a small ESP32-C3 companion display for Bambu Lab printers exposed through Home Assistant entities. It uses a round GC9A01 TFT display, connects to Home Assistant over REST and WebSocket, and shows printer state, progress, finish time, temperatures, layers, and filament information.

The device includes a local web configuration portal, WiFi scanning, Home Assistant entity search, timezone selection, OTA support, and a factory reset option.

## Hardware

- ESP32-C3 development board
- 1.28 inch round TFT display with GC9A01 driver
- USB data cable for flashing
- 3.3 V logic wiring

## Display Wiring

The default firmware pin mapping is:

| Display Pin | ESP32-C3 Pin |
| --- | --- |
| CS | GPIO 7 |
| DC | GPIO 2 |
| RST | GPIO 3 |
| SCLK | GPIO 4 |
| MOSI | GPIO 6 |

Adjust the pin definitions near the top of `circle_bambu_monitor.ino` if your board is wired differently.

## Arduino Libraries

Install these libraries in Arduino IDE:

- WiFi
- WebServer
- Preferences
- HTTPClient
- ArduinoOTA
- ArduinoJson
- ArduinoWebsockets
- SPI
- Adafruit GFX Library
- Adafruit GC9A01A
- LVGL

You also need the ESP32 board support package installed in Arduino IDE.

## First Setup

1. Flash the firmware to the ESP32-C3.
2. If no WiFi is configured, the device starts a setup access point:
   - SSID: `CBM`
   - Password: `circlebambu`
   - IP address: `192.168.4.1`
3. Open `http://192.168.4.1` in a browser.
4. Enter WiFi, Home Assistant URL, and a long-lived access token.
5. Save and let the device restart.
6. Reconnect to your normal WiFi network, find the device IP address in your router or network device list, then open the configuration page again.
7. Select Home Assistant printer entities using the autocomplete fields.

## Home Assistant

The firmware expects printer data to be available as Home Assistant entities. The exact entity names depend on your Bambu Lab integration and language settings.

Common entity types include:

- current phase
- print status
- print progress
- active filament slot
- finish time
- nozzle temperature
- bed temperature
- current layer
- total layers
- filament weight

## Web Installer

This repository includes a starter ESP Web Tools installer in `web-installer/`. It is not complete until compiled firmware binaries are exported and placed into `web-installer/firmware/`.

See [Web Flasher Setup](docs/web-flasher.md) for the release workflow.

## Security Notes

- The Home Assistant long-lived token is stored on the device.
- Do not expose the device configuration page to the public internet.
- Use a private, trusted WiFi network.
- Change the OTA password after setup if you enable OTA updates.
- Factory reset clears WiFi, Home Assistant, OTA, timezone, and entity settings.

## License

This project is released under the MIT License. See [LICENSE](LICENSE).
