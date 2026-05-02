#!/bin/bash
# IP address of your ESP32
IP_ESP32="192.168.1.109"

# Send the STOP signal so the ESP32 shows the default GIF
curl -G "http://$IP_ESP32/batocera" \
    --data-urlencode "s=STOP" \
    --data-urlencode "g=STOP" > /dev/null 2>&1 &