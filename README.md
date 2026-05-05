# Retro Pixel LED - Recalbox Dedicated Firmware

Retro Pixel LED is now a single-purpose ESP32 firmware for Recalbox arcade cabinets.

It listens for Recalbox game launch events and displays an animated GIF matching the current game on a HUB75 LED matrix. If no matching GIF exists, it scrolls the readable game title instead.

This build intentionally removes the general-purpose features from the original project: no web UI, no FTP server, no MQTT/Home Assistant, no OTA, no clock, no weather, no playlist browser, and no WiFiManager portal. Everything required at runtime is preconfigured from `/config.txt` on the SD card.

## Runtime Behavior

1. Before a game is launched, the panel plays `default.gif`.
2. Recalbox sends the current game to the ESP32 through:

   ```text
   http://ESP32_IP/gif?s=<system>&g=<game>&t=<readable title>
   ```

3. The firmware searches for GIFs matching the ROM basename:

   ```text
   /gif/fbneo/dkong.gif
   /gif/fbneo/dkong_2.gif
   /gif/fbneo/dkong_3.gif
   ```

4. If several GIFs exist for the same game, they rotate every `GIF_INTERVAL_SECONDS`.
5. Between two game GIFs, the firmware scrolls the readable game title once.
6. If no GIF exists for the game, the readable game title scrolls continuously.
7. When Recalbox leaves the game, the panel returns to the default GIF.

The existing Recalbox script sends ROM basenames, so `dkong.zip` becomes `dkong` and matches `dkong.gif`.

## SD Card Layout

Recommended layout:

```text
/config.txt
/default.gif
/gif/
  fbneo/
    dkong.gif
    dkong_2.gif
    dkong_3.gif
    sfiii3.gif
  snes/
    zelda3.gif
```

## `/config.txt`

Create this file at the root of the SD card:

```ini
# WiFi
SSID=YourWifiName
PASSWORD=YourWifiPassword
HOSTNAME=retropixel-recalbox

# Network
DHCP=true
# For static IP, use:
# DHCP=false
# IP=192.168.1.108
# GATEWAY=192.168.1.1
# SUBNET=255.255.255.0
# DNS=192.168.1.1

# LED matrix
BRIGHTNESS=40
PANEL_CHAIN=4
DISPLAY_X_OFFSET=128
VIEWPORT_WIDTH=128

# Playback
GIF_ROOT=/gif
DEFAULT_GIF=/gif/default.gif
GIF_INTERVAL_SECONDS=10
TEXT_SPEED_MS=35
TEXT_PAUSE_MS=250
SHOW_TITLE_BETWEEN_GIFS=true

# Advanced HUB75 tuning
MIN_REFRESH_RATE=120
LATCH_BLANKING=1
I2S_SPEED=2
```

For a standard 128x32 matrix made of two 64x32 panels, use:

```ini
PANEL_CHAIN=2
DISPLAY_X_OFFSET=0
VIEWPORT_WIDTH=128
```

For a 256x32 DMD-style setup where the GIF area is the right 128 pixels, use:

```ini
PANEL_CHAIN=4
DISPLAY_X_OFFSET=128
VIEWPORT_WIDTH=128
```

## Recalbox Installation

Copy the script to Recalbox:

```text
/recalbox/share/userscripts/pixel_recalbox[rungame,rundemo,endgame,enddemo,systembrowsing,start,stop,shutdown,reboot,quit,relaunch,sleep,wakeup].sh
```

Optional Recalbox-side config:

```text
/recalbox/share/system/configs/retropixelled.conf
```

Example:

```sh
IP_ESP32="192.168.1.108"
CURL_TIMEOUT="8"
```

The script follows the official Recalbox EmulationStation userscript contract by parsing `-action`, `-statefile`, and `-param`, while keeping `/tmp/es_state.inf` as fallback.

## HTTP Input

The firmware exposes only the minimal input needed by Recalbox:

```text
GET /gif?s=fbneo&g=dkong&t=Donkey%20Kong
```

Useful test from a PC:

```powershell
curl "http://192.168.1.108/gif?s=fbneo&g=dkong&t=Donkey%20Kong"
```

Status endpoint:

```text
GET /status
```

## Firmware Build

Compile with Arduino CLI:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/RetroPixelLED
```

Current validation build:

```text
Flash: 1,034,438 bytes / 78%
RAM:   73,004 bytes / 22%
```

## Dependencies

* ESP32 Arduino core
* AnimatedGIF
* ESP32-HUB75-MatrixPanel-I2S-DMA

## License

MIT. See [LICENSE](LICENSE).
