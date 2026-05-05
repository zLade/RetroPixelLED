# Changelog - Retro Pixel LED

## [Unreleased]

### Added

* **Recalbox Event Script:** Added a userscript integration that reads `/tmp/es_state.inf` and sends compatible Arcade Mode commands to the existing Retro Pixel LED `/batocera` endpoint.

### Changed

* **Arcade Text Fallback:** Recalbox events now include the readable game title, and Arcade Mode uses it as scrolling text when no exact game GIF is found.
* **FTP SD Access:** Added an optional built-in FTP server for local WinSCP access to the SD card.
* **SD WiFi Bootstrap:** Added optional `/wifi_config.txt` support so WiFi credentials can be preloaded from the SD card before the configuration portal starts.
* **Recalbox-Focused Firmware Trim:** Removed compiled clock, auto-clock, NTP, weather icons, and weather/clock MQTT controls to reduce flash usage and keep the runtime focused on GIF, text, and arcade display.

## [4.0.0] - 2026-03-21

### Added - Playlist & Instant Response Update

* **Dynamic Playlist System:** Added support for multiple custom playlists using `.txt` files in `/playlists`. Users can switch themed collections from the web UI without scanning the entire SD card again.
* **Atomic Render Interruption:** Added the global `interruptPlayback` flag synchronized between cores. Playlist, mode, and Batocera changes now stop the current GIF instantly.
* **Playlist Generator Script v1.0.1:** Added an interactive Windows tool that creates playlist files, detects folders, normalizes ESP32 paths, and creates the SD playlist directory automatically.
* **Playlist NVS Persistence:** The system remembers and restores the last selected playlist after restart.

### Optimized

* **Instant Loading:** Playlist mode skips the `LISTING GIFs...` process and starts playback immediately.
* **Memory Safety:** Optimized `std::vector` usage for dynamic playlist listings in the web UI.

### Fixed

* **File Path Generation:** Fixed duplicated Windows drive letters in generated cache paths.
* **Index Reset:** Fixed `gifCachePosition` staying out of range when switching from a long playlist to a shorter one.

## [3.0.5] - 2026-02-20

### Fixed - Critical Stability Hotfix

* **SD Mutex Integration:** Added semaphore protection in file listing and file manager paths to avoid `Guru Meditation Error: LoadProhibited` while browsing the web UI during GIF playback.
* **POST Config Method:** Migrated configuration saving from `GET` to `POST` to avoid truncated URLs and flash write failures.
* **External CSS:** Moved visual styles to `/style.css` to reduce RAM usage and allow browser caching.

## [3.0.4] - 2026-02-19

### Added - Smart Feature & Web Stability Update

* **Automatic Clock Interleaving:** Added a configurable clock display interval during GIF playback.
* **Chunked Transfer Encoding:** Added chunked web responses so large folder and file lists load without exhausting ESP32 RAM.

### Optimized

* **Unified UI:** Moved Auto Clock controls into the Gallery Settings card.
* **HTML Buffer Cleanup:** Refactored `handleRoot` and `handleConfig` to avoid duplicate cards and keep MQTT and hardware settings rendering clean.

### System Improvements

* **MQTT Robustness:** State synchronization runs after saving configuration so Home Assistant reflects changes immediately.
* **Memory Stability:** Reduced heap peak during web UI use by streaming content in chunks.

## [3.0.3] - 2026-02-10

### Added - Performance & Efficiency Update

* **Eco-Energy Mode:** Added dynamic CPU frequency scaling. The ESP32 drops from 240 MHz to 80 MHz when the panel is powered off.
* **Ultra-Fast Wake:** Restores 240 MHz immediately when the panel is turned back on.

### Optimized

* **Arcade Visual Fix:** Removed the intrusive `FILES MODE` label during remote Batocera commands.
* **Task Handling:** Refined `TaskDisplay` priority to reduce stutter during frequency changes.

### Compatibility

* **Core Stability:** Validated against ESP32 Arduino Core 3.3.5.

## [3.0.1] - 2026-01-27

### Fixed - True Random Engine Hotfix

* **Hardware RNG Integration:** Replaced software `randomSeed()` and `random()` seeding with `esp_random()`.
* **SD Pointer Optimization:** Improved cache line alignment so random mode reads complete valid paths.

## [3.0.0] - 2026-01-25

### Added - Infinite SD Engine

* **SD Streaming Engine:** Added direct GIF path streaming from SD cache files.
* **Status UI:** Added the `LISTING GIFs...` status screen.
* **Non-Blocking Process:** Added `yield()` during scans to keep the web server responsive.
* **Instant Interruption:** Improved GIF playback interruption for configuration changes and new searches.

### Optimized

* **Zero RAM Footprint:** Replaced RAM-heavy GIF path vectors with file seek positions where applicable.
* **Signature System:** Detects folder configuration changes to avoid unnecessary scans.
* **OTA Robustness:** Stops the second-core display task during firmware writes.

## [2.2.9] - 2026-01-25

### Added

* **Dynamic Clock Control:** Added variable `startY` positioning depending on MQTT notification state.
* **8 Clock Styles:** Added Rainbow, Solid Neon, Pulse Breath, Matrix Digital, and Gradient modes.
* **Hardware Tuning:** Added web-configurable I2S speed, refresh rate, and latch blanking.
* **Hybrid WiFi Mode:** Added offline operation without network-search blocking.
* **Home Assistant Pro Integration:** Added weather icons, temperature, and dynamic text through MQTT.

### Optimized

* **Panel Stability:** Refactored the drawing engine to remove visual glitches.
* **Memory Management:** Switched to `Minimal SPIFFS` partitions for the 1.2 MB firmware binary.
* **MQTT Synchronization:** GIF mode changes are now instant through forced index reset and GIF object close.

### Fixed

* **Color Persistence:** Fixed 24-bit color conversion for Text and Neon modes.
* **Screen Refresh:** Removed visual leftovers when changing clock effects or minutes.

## [2.1.9] - 2026-01-10

### Added

* **Arcade Mode:** Initial Batocera script integration.
* **Dual Core Engine:** First stable implementation using `vTaskCreatePinnedToCore`.
* **FileManager:** Web file upload.

### Optimized

* **Mutex System:** Added basic SD card access protection.
