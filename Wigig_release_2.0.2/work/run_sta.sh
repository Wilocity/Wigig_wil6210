#!/bin/sh

cd /root/work/
. ./wilocity_rc.sh
echo "General...run SAT mode, stop NM and Firewall"
wilo_remove
wilo_kill_by_keyword wpa_supplicant
wilo_kill_by_keyword hostapd
wilo_stop_nm
wilo_stop_firewall
insmod /usr/lib/modules/$(uname -r)/kernel/net/wireless/cfg80211.ko 
iw reg set "US"
wilo_load_sta
echo "60ad STA was started"
echo "To connect use wilo_scan & wilo_connect SSID"
