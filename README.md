# Retro Pixel LED v4.0.0

### **[Join the Retro Pixel LED Telegram Group](https://t.me/RetroPixelLed)**

## Project Description

**Retro Pixel LED** is advanced firmware for ESP32 devices that control HUB75 LED matrix panels, such as P2.5, P4, and similar RGB matrix panels, through a full web interface.

It turns an LED matrix into a retro information and artwork display with animated GIF playback, scrolling text, an NTP-synchronized clock, Batocera/RetroPie arcade integration, Home Assistant control, and SD card file management.

Version **4.0.0** introduces **dynamic playlists**, allowing instant switching between themed GIF collections without waiting for a full SD card re-index.

A **Lite** version is also available here: [RetroPixelLED-Lite](https://github.com/fjgordillo86/RetroPixelLED-Lite/tree/main).

> [!IMPORTANT]
> **GIF listing:** In GIF Gallery mode, when using **Auto-generate** as the GIF source, folders with hundreds or thousands of GIFs require an initial SD card index. If the panel shows **"LISTING GIFs..."**, it is not frozen. It is building the cache file used for fast playback. Indexing roughly 3000 GIFs can take **7 to 9 minutes**, depending on SD card speed. Do not restart the device during this process.

> [!TIP]
> **No more waiting:** Playlist mode lets the panel load `.txt` playlists generated on a PC. When a playlist is selected instead of Auto-generate, the panel starts playback instantly, even with thousands of files.

## What's New in 4.0.0

| Feature | Technical Detail | Benefit |
| :--- | :--- | :--- |
| **Dynamic Playlists** | `.txt` playlist support in `/playlists`. | Switch themed GIF collections instantly from the web UI. |
| **Instant Switching** | Hardware-level playback interruption through `interruptPlayback`. | Mode, playlist, and Batocera changes stop the current GIF immediately. |
| **PC Playlist Tool** | Optimized interactive Windows `.bat` script. | Create clean ESP32-ready playlists in seconds. |
| **NVS Persistence** | Active playlist is stored in ESP32 flash memory. | The panel remembers the selected mode or playlist after restart. |
| **Auto Clock Interval** | Timed interruption cycle. | Show the clock every X GIFs without manually changing mode. |
| **External CSS** | Web style moved to `/style.css` on the SD card. | Frees RAM and allows browser caching. |
| **Eco-Energy Mode** | Dynamic frequency scaling between 80 MHz and 240 MHz. | Reduces heat and consumption while the panel is off. |

## Core Features

* **Playlist System:** Create playlist text files on the SD card with exact GIF paths and switch between them from the web UI.
* **Real-Time Interruption:** Web, brightness, mode, playlist, and Batocera changes are applied atomically between ESP32 cores.
* **Auto Clock Logic:** The panel can interrupt GIF playback every configurable number of GIFs, display the clock for 10 seconds, then resume the gallery.
* **Smart Web Engine:** Chunked transfer encoding sends large pages and folder lists without exhausting ESP32 RAM.
* **Smart Energy Management:** CPU speed drops from 240 MHz to 80 MHz when the matrix is off, while WiFi and Home Assistant remain available.
* **Dual Core Engine:** Core 0 handles WiFi, Web, and MQTT. Core 1 handles GIF decoding and rendering.
* **True Random Engine:** Uses the ESP32 hardware random generator instead of predictable software randomness.
* **Infinite GIF List:** Reads GIF paths directly from SD cache files to support very large collections.
* **Arcade Mode:** Native Batocera/RetroPie integration. The panel changes GIFs according to the selected or launched game.
* **FileManager Pro:** Upload, delete, and organize GIFs through the web UI without removing the Micro SD card.
* **SD Mutex:** Protects SD card access between cores.

## Bill of Materials

Recommended tested components:

* **Microcontroller:** [ESP32 DevKit V1, 30 pins](https://es.aliexpress.com/item/1005005704190069.html)
* **LED Matrix Panel:** [P2.5 / P4 RGB Matrix Panel](https://es.aliexpress.com/item/1005007439017560.html)
* **Card Reader:** [Micro SD SPI Adapter Module](https://es.aliexpress.com/item/1005005591145849.html)
* **ESP32-to-Panel Board:** [DMDos Board V3 by Mortaca](https://www.mortaca.com/) (optional, no soldering required, includes SD reader)
* **Power Supply:** 5 V supply, with at least 4 A recommended for 64x32 panels.

## Installation and Configuration

### 1. Wiring

If you use a DMDos Board V3, this wiring is already handled and you can skip to the next step.

#### Micro SD Card Reader (SPI)

| SD Pin | ESP32 Pin | Function |
| :--- | :--- | :--- |
| **CS** | GPIO 5 | Chip Select |
| **CLK** | GPIO 18 | Clock |
| **MOSI** | GPIO 23 | Master Out Slave In |
| **MISO** | GPIO 19 | Master In Slave Out |
| **VCC** | 3.3 V | Power |
| **GND** | GND | Ground |

#### HUB75 RGB LED Panel

| Panel Pin | ESP32 Pin | Function |
| :--- | :--- | :--- |
| **R1** | GPIO 25 | Upper red data |
| **G1** | GPIO 26 | Upper green data |
| **B1** | GPIO 27 | Upper blue data |
| **R2** | GPIO 14 | Lower red data |
| **G2** | GPIO 12 | Lower green data |
| **B2** | GPIO 13 | Lower blue data |
| **A** | GPIO 33 | Row select A |
| **B** | GPIO 32 | Row select B |
| **C** | GPIO 22 | Row select C |
| **D** | GPIO 17 | Row select D |
| **E** | GND | Ground |
| **CLK** | GPIO 16 | Clock |
| **LAT** | GPIO 4 | Latch |
| **OE** | GPIO 15 | Output Enable / brightness |

### 2. Flash the ESP32

Arduino IDE is no longer required for normal installation. You can flash the ESP32 directly from a compatible browser.

### **[Open the Retro Pixel LED web installer](https://fjgordillo86.github.io/RetroPixelLED/)**

1. Use **Google Chrome** or **Microsoft Edge**.
2. Connect the ESP32 to your computer over USB.
3. Click **Install** and select the correct COM port.
4. Enable **Erase device** in the installer wizard to fully clean flash memory and avoid fragmentation errors.

If no COM port appears, install the USB driver for your board:

* **CP2102:** [Silicon Labs drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
* **CH340/CH341:** [SparkFun CH340 driver guide](https://learn.sparkfun.com/tutorials/how-to-install-ch340-drivers/all)

When flashing from **Arduino IDE**, the firmware requires the correct partition scheme:

1. Open **Tools > Partition Scheme**.
2. Select **Minimal SPIFFS (Large APPS with OTA)**.
3. The first flash must be done over USB so the partition layout is applied. Later updates can be done over OTA.

## SD Card Layout

Format the card as **FAT32** and keep this structure:

```text
/ (SD root)
├── gifs/                       <-- Used by GIF Gallery mode.
│   ├── Arcade/                 <-- GIFs organized by category.
│   └── Consoles/               <-- GIFs organized by category.
├── playlists/                  <-- Playlists generated by the playlist tool.
│   ├── My Favorites.txt
│   ├── Metal Slug.txt
│   └── All.txt
├── Batocera/                   <-- Used by Arcade mode.
│   ├── default/                <-- Contains _default.gif.
│   ├── mame/                   <-- GIFs organized by system.
│   └── neogeo/                 <-- GIFs organized by system.
├── Playlist Generator.bat
├── batocera_cache.txt          <-- Generated by the arcade script.
├── gif_cache.txt               <-- Generated by the firmware.
└── gif_cache.sig               <-- Generated validation signature.
```

## First WiFi Setup

On first boot, or after changing networks, Retro Pixel LED starts a captive configuration portal:

1. Connect to the WiFi network named `Retro Pixel LED`.
2. The captive portal should open automatically. If not, browse to `192.168.4.1`.
3. Select your home WiFi, enter the password, and save. The ESP32 restarts and joins your local network.

## Web UI

After the device joins your local network, open its IP address in a browser.

Available controls:

* Real-time mode switching between **GIF Gallery**, **Clock**, **Scrolling Text**, and **Arcade**.
* LED brightness control from 0 to 100%.
* SD file browser for uploading, deleting, and organizing GIFs.
* Scrolling text editor with color and speed controls.
* Home Assistant integration.
* OTA firmware update.

## Playlist Generator Script (Windows)

The script `Playlist Generator v1.0.1.bat` in the `tools` folder creates playlist files without manually editing paths.

1. Place the `.bat` file at the SD card root, next to the `gifs` folder.
2. Run the script.
3. Select GIF folders by typing their numbers separated by commas, or type `ALL`.
4. Enter the playlist name.
5. The script creates `/playlists/<name>.txt` with ESP32-ready paths.
6. Insert the SD card into Retro Pixel LED and select the playlist in the web UI.

## Batocera Integration

Arcade Mode turns the LED matrix into a dynamic arcade marquee that reacts to the game selected or launched in Batocera.

The system does not choose a random GIF in this mode. It searches for the exact GIF matching the launched game. If it is missing, it falls back to the system logo, then to `_default.gif`.

### Arcade GIF Indexer Script

The Windows script in `tools` prepares the SD card and synchronizes ROM names with GIF names.

1. Connect the Retro Pixel LED SD card to your computer.
2. Run `Arcade GIF Indexer v1.5.4.bat`.
3. Enter the SD drive letter, for example `E`.
4. Enter the Batocera ROM path:
   * Local path example: `D:\share\roms`
   * Network path example: `\\192.168.1.112\share\roms`
5. The script creates folders by system in `/Batocera/`.
6. It writes a name reference file next to the script so you know which GIF filenames to create.
7. After copying GIFs into the system folders, run the script again to generate `batocera_cache.txt`.

GIF naming rules:

* If the ROM is `mslug.zip`, the GIF must be `mslug.gif`.
* Put a system fallback logo named `_logo.gif` inside each system folder.
* Put the global fallback GIF named `_default.gif` inside `/Batocera/default/`.

Default Batocera Samba credentials:

* **Username:** `root`
* **Password:** `linux`

### Batocera Event Scripts

Install the three scripts from `batocera/scripts` into Batocera:

* `/game-start/pixel_start.sh`
* `/game-end/pixel_stop.sh`
* `/quit/pixel_off.sh`

Edit the ESP32 IP address in each script before copying them.

Do not edit the scripts with basic Windows Notepad because it can change line endings to CRLF. Use Notepad++, VS Code, or Sublime Text and keep Unix LF line endings.

Grant execution permissions over SSH:

```bash
chmod +x /userdata/system/configs/emulationstation/scripts/game-start/pixel_start.sh
chmod +x /userdata/system/configs/emulationstation/scripts/game-end/pixel_stop.sh
chmod +x /userdata/system/configs/emulationstation/scripts/quit/pixel_off.sh
```

Verify permissions:

```bash
ls -l /userdata/system/configs/emulationstation/scripts/game-start/pixel_start.sh
ls -l /userdata/system/configs/emulationstation/scripts/game-end/pixel_stop.sh
ls -l /userdata/system/configs/emulationstation/scripts/quit/pixel_off.sh
```

### Fixed ESP32 IP Address

Batocera scripts send commands to one configured IP address. Reserve a fixed IP for the ESP32 in your router DHCP settings so the integration keeps working after router restarts.

## Recalbox Integration

Recalbox can use the same Retro Pixel LED Arcade Mode endpoint as Batocera. The firmware route is still named `/batocera`, and it still reads `/batocera_cache.txt`, but the Recalbox script sends compatible `system` and `game` values from `/tmp/es_state.inf`.

Use the same SD card GIF layout and naming rules described above:

* Put arcade GIFs in `/Batocera/<system>/`.
* Keep `_logo.gif` as the system fallback and `/Batocera/default/_default.gif` as the global fallback.
* Generate `batocera_cache.txt` with the Arcade GIF Indexer. You can point the ROM path to your Recalbox `share/roms` folder.

### Recalbox Event Script

Install the script from `recalbox/scripts` into Recalbox:

```bash
mkdir -p /recalbox/share/userscripts
cp pixel_recalbox.sh "/recalbox/share/userscripts/pixel_recalbox[rungame,rundemo,endgame,enddemo,systembrowsing,gamelistbrowsing,start,stop,shutdown,reboot,quit,relaunch,sleep,wakeup].sh"
chmod +x "/recalbox/share/userscripts/pixel_recalbox[rungame,rundemo,endgame,enddemo,systembrowsing,gamelistbrowsing,start,stop,shutdown,reboot,quit,relaunch,sleep,wakeup].sh"
```

The event filter in the filename follows the official Recalbox userscript syntax and avoids running the script for unrelated events. You can keep a simpler `pixel_recalbox.sh` filename, but Recalbox will then call it for every EmulationStation event.

Edit the ESP32 IP address directly in the script:

```bash
IP_ESP32="192.168.1.109"
```

You can also keep the script unchanged and create this optional Recalbox config file:

```bash
nano /recalbox/share/system/configs/retropixelled.conf
```

```bash
IP_ESP32="192.168.1.109"
CURL_TIMEOUT="8"
```

How it maps Recalbox events:

* `rungame`, `rundemo`: sends the ROM folder system and the ROM filename without extension to Retro Pixel LED.
* `wakeup`: sends the current game again only if Recalbox reports `State=playing` or `State=demo`; otherwise sends `STOP`.
* `endgame`, `enddemo`, `systembrowsing`, `gamelistbrowsing`, `start`, `runkodi`, `endkodi`, `sleep`, `relaunch`: sends `STOP`, so the panel loads the default arcade GIF.
* `stop`, `shutdown`, `reboot`, `quit`: sends `OFF`, so the panel returns to normal GIF mode.

Do not edit the script with basic Windows Notepad because it can change line endings to CRLF. Use Notepad++, VS Code, or Sublime Text and keep Unix LF line endings.

## Home Assistant Integration

Retro Pixel LED integrates through **MQTT Discovery**. After configuring your MQTT broker in the web UI, the device appears automatically in Home Assistant.

Available entities:

* `switch.retro_pixel_led_state`: turn the matrix on or off.
* `select.retro_pixel_led_mode`: select `GIFs`, `Clock`, `Text`, or `Arcade`.
* `number.retro_pixel_led_brightness`: brightness control from 0 to 255.
* `select.retro_pixel_led_clock_style`: select one of the clock styles.
* `light.retro_pixel_led_clock_color`: RGB clock color.
* `text.retro_pixel_led_display_text`: scrolling text message.
* `light.retro_pixel_led_text_color`: RGB text color.

Weather and notification topics use the device ID:

* `retropixel/retropixel_ID/cmd/weather`
* `retropixel/retropixel_ID/cmd/temp`
* `retropixel/retropixel_ID/cmd/notify`

The ID is the last six characters of the ESP32 MAC address and appears in the serial monitor.

### Weather Icon IDs

| ID | State | Icon |
| :--- | :--- | :--- |
| **0** | Clear / Sun | Sun |
| **1** | Cloudy | Cloud |
| **2** | Rain | Rain cloud |
| **3** | Snow | Snow |
| **4** | Storm | Lightning cloud |
| **5** | Night | Moon |
| **6** | Thunderstorm rain | Lightning and rain |
| **7** | Fog | Fog |
| **Default** | Default | Sun |

### Home Assistant YAML Example

```yaml
alias: Update Retro Pixel LED Weather
description: Send temperature and weather icons to Retro Pixel LED
triggers:
  - entity_id: sensor.aemet_temperature
    trigger: state
  - entity_id: weather.aemet
    trigger: state
actions:
  - data:
      topic: retropixel/retropixel_ID/cmd/temp
      payload: "{{ states('sensor.aemet_temperature') | round(0) }}"
    action: mqtt.publish
  - data:
      topic: retropixel/retropixel_ID/cmd/weather
      payload: >
        {% set state = states('weather.aemet') %}
        {% if state == 'sunny' %} 0
        {% elif state == 'cloudy' or state == 'partlycloudy' %} 1
        {% elif state == 'rainy' or state == 'pouring' %} 2
        {% elif state == 'snowy' or state == 'snowy-rainy' %} 3
        {% elif state == 'lightning' %} 4
        {% elif state == 'clear-night' %} 5
        {% elif state == 'lightning-rainy' %} 6
        {% elif state == 'fog' %} 7
        {% else %} 0
        {% endif %}
    action: mqtt.publish
```

Notification script example:

```yaml
alias: Retro Pixel LED Notification
sequence:
  - data:
      topic: retropixel/retropixel_ID/cmd/notify
      payload: Home Assistant notification
    action: mqtt.publish
mode: single
icon: mdi:cellphone-sound
```

## Performance Cache

The firmware avoids full SD scans on every boot by using a validation signature:

1. The selected folders are written into a signature file.
2. If the selected folders have not changed after restart, `gif_cache.txt` is reused.
3. If the signature changed, the SD card is scanned again and the cache is rebuilt.

## Roadmap

Performance improvements:

* Batocera marquee download and automatic resizing.
* Binary search for `batocera_cache.txt` and `gif_cache.txt`.
* Direct SD-to-web streaming in FileManager to remove intermediate RAM buffers.

Visual improvements:

* Rotating playlist for Arcade Mode while a game is active.
* Multiple GIF variants per game, such as `sonic_1.gif` and `sonic_2.gif`.
* More text sizes.
* Infrared remote control for power, brightness, and modes.

## Required Libraries

Install these libraries when compiling from Arduino IDE:

* [ESP32-HUB75-MatrixPanel-I2S-DMA](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA)
* [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF)
* [WiFiManager](https://github.com/tzapu/WiFiManager)
* [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library)
* [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

The **SD** and **FS** libraries are included in the ESP32 Arduino core.

## License and Credits

This project is released under the MIT License. See [LICENSE](LICENSE).

Special thanks to:

* **ESP32-HUB75-MatrixPanel-I2S-DMA**
* **AnimatedGIF**
* **WiFiManager**
* **DMDos Telegram community**
* **RpiTeam** for the GIF collection: [Neo-Arcadia forum thread](https://www.neo-arcadia.com/forum/viewtopic.php?t=67065)
* **joseAveleira** for the weather notification idea in the clock: [RelojPixel](https://github.com/joseAveleira/RelojPixel/tree/main/src)
