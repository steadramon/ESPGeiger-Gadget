# ESPGeiger Gadget

A LAN companion display for [ESPGeiger](https://github.com/steadramon/ESPGeiger) radiation detectors. Auto-discovers stations on the network via mDNS, subscribes to their live UDP click streams, and renders per-station CPM, sparklines, µSv/h, and 24-hour history on a 2.8" touch screen. Runs on a Sunton ESP32-2432S028R "Cheap Yellow Display".

<p align="center">
  <img src="docs/screenshots/main.png" alt="Main station list — discovered ESPGeiger stations with live CPM, IP, and last-seen age" width="240">
  &nbsp;&nbsp;
  <img src="docs/screenshots/detail.png" alt="Detail screen — big colour-coded CPM, uSv/h, 60-second and 60-minute sparklines, 24-hour history bar chart" width="240">
</p>

## Hardware

Sunton ESP32-2432S028R — 240×320 SPI panel, resistive touch, speaker, USB-C. [from AliExpress](https://s.click.aliexpress.com/e/_c3zLYcL3) (with or without case).

Both ILI9341 (Rv2) and ST7789 (Rv3) panel revisions are supported; flash whichever PlatformIO env matches the board.

## Build and flash

```sh
pio run -e cyd                  # ST7789 (Rv3), release
pio run -e cyd_ili9341          # ILI9341 (Rv2)
pio run -e cyd_debug            # with serial logging
pio run -e cyd -t upload        # flash via USB
pio run -e cyd -t monitor       # serial monitor (115200)
```

First boot shows a WiFi picker; pick an SSID, enter the password on the on-screen keyboard, and the gadget reconnects automatically thereafter. Everything else (theme, hostname, audio, watchdogs, station cap) lives under the gear icon and persists to NVS.

Subsequent firmware updates can be uploaded over the network at `http://<hostname>.local/update` — the dual-partition layout (1.9 MB OTA slots) automatically rolls back if the new firmware crashes within 30 s of boot.

### Pre-built firmware

If you don't want to set up PlatformIO, the [Releases page](../../releases) has pre-built binaries for both panel variants. **Count the USB ports on your board** to pick the right one:

| USB ports | Panel | Grab this `-merged.bin` |
|-----------|-------|-------------------------|
| Two (micro-USB **and** USB-C) | ST7789 (Rv3) | `espgadget-cyd-st7789-rv3-vX.Y.Z-merged.bin` |
| One (micro-USB only) | ILI9341 (Rv2) | `espgadget-cyd-ili9341-rv2-vX.Y.Z-merged.bin` |

Stickers on the board (e.g. `TPM408`) are batch labels and do not identify the panel. If you guess wrong nothing breaks: the screen just boots blank or garbled, so flash the other file instead.

```sh
esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX --baud 921600 \
  write_flash 0x0 espgadget-cyd-st7789-rv3-vX.Y.Z-merged.bin
```

After that the smaller `.bin` (without `-merged`) is the OTA file.

## Acknowledgements

- [rzeldent/esp32-smartdisplay](https://github.com/rzeldent/esp32-smartdisplay) and the [Sunton board JSONs](https://github.com/rzeldent/platformio-espressif32-sunton).
- [LVGL](https://lvgl.io/) and [LovyanGFX](https://github.com/lovyan03/LovyanGFX).

## License

GPLv3, matching the parent ESPGeiger firmware.
