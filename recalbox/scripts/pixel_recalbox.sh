#!/bin/sh

# Retro Pixel LED integration for Recalbox EmulationStation events.
# Copy this file to /recalbox/share/userscripts and edit IP_ESP32.

SCRIPT_VERSION="pixel_recalbox.sh:v1.0.0"

# IP address of your ESP32 running Retro Pixel LED.
IP_ESP32="192.168.1.108"

# Recalbox passes the state file path with -statefile. This default is kept
# for manual runs and older setups.
STATE_FILE="/tmp/es_state.inf"
CLI_ACTION=""
CLI_PARAM=""

# Optional config override. Create this file with shell variables such as:
# IP_ESP32="192.168.1.108"
# CURL_TIMEOUT="8"
CONFIG_FILE="/recalbox/share/system/configs/retropixelled.conf"

# Keep the request bounded. The curl call runs in the background, and some
# ESP32/network combinations need more than two seconds to answer.
CURL_TIMEOUT="8"

if [ -f "$CONFIG_FILE" ]; then
  # shellcheck source=/dev/null
  . "$CONFIG_FILE"
fi

log() {
  logger -t retropixelled-recalbox "$SCRIPT_VERSION $1" 2>/dev/null || true
}

get_val() {
  if [ ! -f "$STATE_FILE" ]; then
    return 0
  fi

  grep "^$1=" "$STATE_FILE" | cut -d'=' -f2- | tr -d '\r' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}

to_lower() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

is_empty_value() {
  [ -z "$1" ] || [ "$1" = "null" ]
}

parse_args() {
  while [ "$#" -gt 0 ]; do
    key="$1"
    shift

    case "$key" in
      -action)
        if [ "$#" -gt 0 ]; then
          CLI_ACTION="$1"
          shift
        fi
        ;;
      -statefile)
        if [ "$#" -gt 0 ]; then
          STATE_FILE="$1"
          shift
        fi
        ;;
      -param)
        if [ "$#" -gt 0 ]; then
          CLI_PARAM="$1"
          shift
        fi
        ;;
    esac
  done
}

rom_basename_without_extension() {
  rom_path="$1"
  raw_name="${rom_path##*/}"
  raw_name="${raw_name%.*}"

  printf '%s' "$raw_name" | sed 's/\\//g'
}

game_title_fallback() {
  title="$1"

  # Remove common collection numbering such as "001 Sonic".
  title="$(printf '%s' "$title" | sed 's/^[0-9][0-9][0-9][[:space:]]*//')"

  printf '%s' "$title" | sed 's/\\//g'
}

system_from_rom_path() {
  printf '%s' "$1" | sed -n 's#.*[/\\]roms[/\\]\([^/\\]*\)[/\\].*#\1#p'
}

send_to_panel() {
  panel_system="$1"
  panel_game="$2"
  panel_title="$3"

  if [ -z "$IP_ESP32" ]; then
    log "ESP32 IP address is empty; request skipped"
    return 1
  fi

  if is_empty_value "$panel_title"; then
    curl -s -G \
      --connect-timeout "$CURL_TIMEOUT" \
      --max-time "$CURL_TIMEOUT" \
      --data-urlencode "s=$panel_system" \
      --data-urlencode "g=$panel_game" \
      "http://$IP_ESP32/batocera" > /dev/null 2>&1 &
  else
    curl -s -G \
      --connect-timeout "$CURL_TIMEOUT" \
      --max-time "$CURL_TIMEOUT" \
      --data-urlencode "s=$panel_system" \
      --data-urlencode "g=$panel_game" \
      --data-urlencode "t=$panel_title" \
      "http://$IP_ESP32/batocera" > /dev/null 2>&1 &
  fi
}

send_game() {
  game_system="$1"
  game_name="$2"
  game_title="$3"

  if is_empty_value "$game_system" || is_empty_value "$game_name"; then
    log "missing system or game; loading default arcade GIF"
    send_to_panel "STOP" "STOP"
    return
  fi

  log "game start: system=$game_system game=$game_name title=$game_title"
  send_to_panel "$game_system" "$game_name" "$game_title"
}

send_stop() {
  log "game stop or browsing; loading default arcade GIF"
  send_to_panel "STOP" "STOP"
}

send_off() {
  log "shutdown event; returning panel to GIF mode"
  send_to_panel "OFF" "OFF"
}

parse_args "$@"

if [ ! -f "$STATE_FILE" ]; then
  log "$STATE_FILE not found; using launch arguments only"
fi

STATE_ACTION="$(to_lower "$(get_val "Action")")"
ACTION="$(to_lower "$CLI_ACTION")"
if is_empty_value "$ACTION"; then
  ACTION="$STATE_ACTION"
fi
if ! is_empty_value "$STATE_ACTION" && [ "$STATE_ACTION" != "$ACTION" ]; then
  log "state action mismatch: cli=$ACTION state=$STATE_ACTION"
fi

ACTION_PARAM="$CLI_PARAM"
if is_empty_value "$ACTION_PARAM"; then
  ACTION_PARAM="$(get_val "ActionData")"
fi

ROM="$ACTION_PARAM"
if is_empty_value "$ROM"; then
  ROM="$(get_val "GamePath")"
fi
GAME_NAME="$(get_val "Game")"
SYSTEM_ID="$(get_val "SystemId")"
SYSTEM_NAME="$(get_val "System")"
STATE="$(to_lower "$(get_val "State")")"

# SystemId is the Recalbox short id and normally matches the ROM folder
# used by the Retro Pixel LED Batocera cache, for example snes or neogeo.
SYSTEM="$(system_from_rom_path "$ROM")"
if is_empty_value "$SYSTEM"; then
  SYSTEM="$SYSTEM_ID"
fi
if is_empty_value "$SYSTEM"; then
  SYSTEM="$SYSTEM_NAME"
fi

# The firmware cache is built from ROM names, so GamePath is preferred over
# the display title from gamelist metadata.
GAME="$(rom_basename_without_extension "$ROM")"
if is_empty_value "$GAME"; then
  GAME="$(game_title_fallback "$GAME_NAME")"
fi
GAME_TITLE="$(game_title_fallback "$GAME_NAME")"
if is_empty_value "$GAME_TITLE"; then
  GAME_TITLE="$GAME"
fi

case "$ACTION" in
  rungame|rundemo)
    send_game "$SYSTEM" "$GAME" "$GAME_TITLE"
    ;;
  wakeup)
    if [ "$STATE" = "playing" ] || [ "$STATE" = "demo" ]; then
      send_game "$SYSTEM" "$GAME" "$GAME_TITLE"
    else
      send_stop
    fi
    ;;
  endgame|enddemo|systembrowsing|start|runkodi|endkodi|sleep|relaunch)
    send_stop
    ;;
  gamelistbrowsing)
    log "ignored browsing event: $ACTION"
    ;;
  stop|shutdown|reboot|quit)
    send_off
    ;;
  *)
    log "ignored action: $ACTION"
    ;;
esac

exit 0
