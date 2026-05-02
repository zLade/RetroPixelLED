#!/bin/bash
# IP address of your ESP32
IP_ESP32="192.168.1.109"

# Send a special request to return to the desired mode (e.g.: mode 1 - GIFs)
curl -G "http://$IP_ESP32/batocera" \
    --data-urlencode "s=OFF" \
    --data-urlencode "g=OFF" > /dev/null 2>&1 &