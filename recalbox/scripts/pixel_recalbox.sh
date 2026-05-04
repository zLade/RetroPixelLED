#!/bin/bash

# Retro Pixel LED integration for Recalbox EmulationStation events.
# Copy this file to /recalbox/share/userscripts and edit IP_ESP32.

SCRIPT_VERSION="pixel_recalbox.sh:v1.0.0"

# IP address of your ESP32 running Retro Pixel LED.
IP_ESP32="192.168.1.109"

# Recalbox writes the current EmulationStation event here before running
# userscripts.
STATE_FILE="/tmp/es_state.inf"

# Optional config override. Create this file with shell variables such as:
# IP_ESP32="192.168.1.109"
# CURL_TIMEOUT="2"
CONFIG_FILE="/recalbox/share/system/configs/retropixelled.conf"

# Keep requests short so EmulationStation is never blocked for long.
CURL_TIMEOUT="2"

if [ -f "$CONFIG_FILE" ]; then
  # shellcheck disable=SC1090
  . "$CONFIG_FILE"
fi

log() {
  logger -t retropixelled-recalbox "$SCRIPT_VERSION $1" 2>/dev/null || true
}

get_val() {
  grep "^$1=" "$STATE_FILE" | cut -d'=' -f2- | tr -d '\r' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}

rom_basename_without_extension() {
  local rom_path="$1"
  local raw_name

  raw_name="${rom_path##*/}"
  raw_name="${raw_name%.*}"
  raw_name="${raw_name//\\/}"

  printf '%s' "$raw_name"
}

game_title_fallback() {
  local title="$1"

  # Remove common collection numbering such as "001 Sonic".
  title="$(printf '%s' "$title" | sed 's/^[0-9][0-9][0-9][[:space:]]*//')"
  title="${title//\\/}"

  printf '%s' "$title"
}

send_to_panel() {
  local system="$1"
  local game="$2"

  if [ -z "$IP_ESP32" ]; then
    log "ESP32 IP address is empty; request skipped"
    return 1
  fi

  curl -s -G \
    --connect-timeout "$CURL_TIMEOUT" \
    --max-time "$CURL_TIMEOUT" \
    --data-urlencode "s=$system" \
    --data-urlencode "g=$game" \
    "http://$IP_ESP32/batocera" > /dev/null 2>&1 &
}

send_game() {
  local system="$1"
  local game="$2"

  if [ -z "$system" ] || [ -z "$game" ]; then
    log "missing system or game; loading default arcade GIF"
    send_to_panel "STOP" "STOP"
    return
  fi

  log "game start: system=$system game=$game"
  send_to_panel "$system" "$game"
}

send_stop() {
  log "game stop or browsing; loading default arcade GIF"
  send_to_panel "STOP" "STOP"
}

send_off() {
  log "shutdown event; returning panel to GIF mode"
  send_to_panel "OFF" "OFF"
}

if [ ! -f "$STATE_FILE" ]; then
  log "$STATE_FILE not found"
  exit 0
fi

ACTION="$(get_val "Action")"
GAME_NAME="$(get_val "Game")"
ROM="$(get_val "GamePath")"
SYSTEM_ID="$(get_val "SystemId")"
SYSTEM_NAME="$(get_val "System")"
ACTION="$(printf '%s' "$ACTION" | tr '[:upper:]' '[:lower:]')"

# SystemId is the Recalbox short id and normally matches the ROM folder
# used by the Retro Pixel LED Batocera cache, for example snes or neogeo.
SYSTEM="$SYSTEM_ID"
if [ -z "$SYSTEM" ] || [ "$SYSTEM" = "null" ]; then
  SYSTEM="$SYSTEM_NAME"
fi

# The firmware cache is built from ROM names, so GamePath is preferred over
# the display title from gamelist metadata.
GAME="$(rom_basename_without_extension "$ROM")"
if [ -z "$GAME" ] || [ "$GAME" = "null" ]; then
  GAME="$(game_title_fallback "$GAME_NAME")"
fi

case "$ACTION" in
  rungame)
    send_game "$SYSTEM" "$GAME"
    ;;
  wakeup)
    if [ -n "$SYSTEM" ] && [ "$SYSTEM" != "null" ] && [ -n "$GAME" ] && [ "$GAME" != "null" ]; then
      send_game "$SYSTEM" "$GAME"
    else
      send_stop
    fi
    ;;
  endgame|systembrowsing|gamelistbrowsing|start|runkodi|endkodi)
    send_stop
    ;;
  stop|shutdown|reboot)
    send_off
    ;;
  *)
    log "ignored action: $ACTION"
    ;;
esac

exit 0
