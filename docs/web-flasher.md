# Web Flasher Setup

Circle Bambu Monitor can be distributed with a browser-based installer using ESP Web Tools.

## Release Flow

1. Open `circle_bambu_monitor/circle_bambu_monitor.ino` in Arduino IDE.
2. Select the ESP32-C3 board and the same board options used for testing.
3. Run `Sketch > Export Compiled Binary`.
4. Locate the generated `.bin` files.
5. Use the generated `circle_bambu_monitor.ino.merged.bin` file if Arduino created one.
6. Copy it into `web-installer/firmware/` using a versioned release name.
7. Update `web-installer/manifest.json` with the firmware filename and version.
8. Host `web-installer/` over HTTPS, for example with GitHub Pages.

For version `0.11.15`, the web installer expects:

```text
web-installer/firmware/circle-bambu-monitor-0.11.15-merged.bin
```

## Merged Binary

ESP Web Tools can flash a single merged image at offset `0`. For ESP32-C3 builds, create that merged image with `esptool.py` using the bootloader, partition table, boot app image, and application binary produced by Arduino.

Example shape:

```bash
esptool.py --chip esp32c3 merge_bin \
  -o web-installer/firmware/circle-bambu-monitor-0.11.15-merged.bin \
  --flash_mode dio \
  --flash_freq 40m \
  --flash_size 4MB \
  0x0000 bootloader.bin \
  0x8000 partitions.bin \
  0xe000 boot_app0.bin \
  0x10000 circle_bambu_monitor.ino.bin
```

The exact filenames and offsets can differ by board package and Arduino settings, so verify them from the Arduino build output before publishing a release. If Arduino already generated a `.merged.bin` file, prefer that file and flash it at offset `0`.

## Hosting Notes

- The installer page must be served over HTTPS or from localhost.
- Chrome and Edge desktop support Web Serial.
- iOS browsers do not support Web Serial.
- Keep release binaries versioned and avoid overwriting old files after publishing.
