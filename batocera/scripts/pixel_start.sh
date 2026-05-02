#!/bin/bash
IP_ESP32="192.168.1.109"

# Clean the system: de /userdata/roms/snes/... keep only "snes"
RAW_SYSTEM="$1"
SYSTEM=$(echo "$RAW_SYSTEM" | awk -F'/' '{print $(NF-1)}')

# Clean the game: remove path, extension, and backslashes
RAW_GAME=$(basename -- "$2")
GAME_WITHOUT_EXT="${RAW_GAME%.*}"
CLEAN_GAME=$(echo "$GAME_WITHOUT_EXT" | sed 's/\\//g')

# Send the cleaned data
curl -s -G \
    --data-urlencode "s=$SYSTEM" \
    --data-urlencode "g=$CLEAN_GAME" \
    "http://$IP_ESP32/batocera" > /dev/null &