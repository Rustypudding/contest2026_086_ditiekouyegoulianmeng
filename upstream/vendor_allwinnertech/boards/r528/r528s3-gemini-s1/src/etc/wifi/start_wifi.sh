#!/bin/sh
# This board's sh is NSH: it has no $1/$2, no VAR=value,
# no grep and no loops, so WiFi setup cannot live here.
# Use the ai_agent CLI instead:
#   ai_agent
#   set_wifi <ssid> <password>
# Boot connect (saved creds, then networks.d, then
# wapi.conf) is done in C by network_wifi_boot_connect().
echo "[wifi] use ai_agent: set_wifi <ssid> <pass>"
