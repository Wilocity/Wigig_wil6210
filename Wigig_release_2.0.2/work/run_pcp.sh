#!/bin/sh

cd /root/work/
. ./wilocity_rc.sh

echo "General...run AP mode, stop NM and Firewall"
wilo_remove
wilo_kill_by_keyword wpa_supplicant
wilo_kill_by_keyword hostapd
wilo_stop_nm
wilo_stop_firewall
insmod /usr/lib/modules/$(uname -r)/kernel/net/wireless/cfg80211.ko 
iw reg set "US"

cd /root/work/wil6210
./wil6210_start -c pcp.hostapconfig &
echo "60ad AP was started"
sleep 2
dmesg -c > /tmp/ap_run.log
#ifconfig wlan0 up 192.168.112.1
#/root/work/hostap/hostapd/hostapd /root/work/hostap/hostapd/hostapd_wifi.conf -d -B &
#echo "11n AP was started"

#echo "----------------------------------------------------" >> /tmp/ap_run.log
#echo "----------------------------------------------------" >> /tmp/ap_run.log

#echo "----------------------------------------------------" >> /tmp/ap_run.log
#echo "----------------------------------------------------" >> /tmp/ap_run.log

#dmesg -c >> /tmp/ap_run.log


#echo "Start FTPD service"

#/bin/systemctl  stop pure-ftpd.service
#/bin/systemctl  start pure-ftpd.service
#/bin/systemctl  status pure-ftpd.service


