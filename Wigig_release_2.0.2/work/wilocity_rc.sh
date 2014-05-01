
WILO_SRCROOT=$PWD

STA_MAC_ADDR=04:CE:14:44:BB:CC
AP_MAC_ADDR=04:CE:14:55:DD:20

WILO_SUBNET=10.0.0
IP_ADDR=99
WILO_DEV_MODE=0

WIGIG_NAME=60ad_AP_1_0_3

WILO_TOOLS_DIR=/usr/local/bin/wilocity
WILO_IMAGES_LIST_DIR=$WILO_TOOLS_DIR/burn_files
WILO_IMAGES_SUBDIR=images
WILO_IMAGES_DIR=$WILO_IMAGES_LIST_DIR/$WILO_IMAGES_SUBDIR

 
wilo_stop_firewall() {
 systemctl stop firewalld.service > /dev/null 2>&1 && return 0
 systemctl stop iptables.service > /dev/null 2>&1 && return 0
echo ERROR: Failed to stop firewall service && return 1
}

wilo_stop_nm() {
test -e /etc/init.d/NetworkManager &&  /etc/init.d/NetworkManager stop && return 0
 /sbin/chkconfig NetworkManager off &&  systemctl stop NetworkManager.service && return 0
echo ERROR: Failed to stop network manager && return 1
}

wilo_start_nm() {
test -e /etc/init.d/NetworkManager &&  /etc/init.d/NetworkManager start && return 0
 systemctl start NetworkManager.service && return 0
echo ERROR: Failed to start network manager && return 1
}

wilo_remove() {
rmmod wil6210 > /dev/null 2>&1 || echo -n
}

wilo_insert() {
test ! 1 -eq $WILO_DEV_MODE || ( cd $WILO_SRCROOT/wil6210 && make ) || return 1

  insmod $WILO_SRCROOT/wil6210/wil6210.ko "$1" && return 0
}
 
wilo_insert_pcp() {
test ! 1 -eq $WILO_DEV_MODE || ( cd $WILO_SRCROOT/wil6210 && make ) || return 1

  insmod $WILO_SRCROOT/wil6210/wil6210.ko use_pcp_for_ap=1 && return 0
}

wilo_kill_by_keyword() {
 kill `ps -ef | grep "$1" | grep -v grep | awk '//{print $2}'` >/dev/null 2>&1 || echo -n
}

wilo_reload_hostapd() {

SSID=$1
test -z $SSID && SSID=wilo_fst_2

test ! 1 -eq $WILO_DEV_MODE || ( cd $WILO_SRCROOT/hostap/hostapd && cp defconfig .config && make -j8 ) || return 1
wilo_kill_by_keyword hostapd_11ad_amended

sed -e "s/WILO_INTERFACE/$WLAN/g" \
    -e "s/WILO_SSID/$SSID/g" < hostap/hostapd/hostapd_11ad.conf > hostap/hostapd/hostapd_11ad_amended.conf
 $WILO_SRCROOT/hostap/hostapd/hostapd -dd hostap/hostapd/hostapd_11ad_amended.conf
}

wilo_reload_wifi_hostapd() {

IFACE=$1
SSID=$2

sed -e "s/WIFI_INTERFACE/$IFACE/g" \
    -e "s/WIFI_SSID/$SSID/g" < hostap/hostapd/hostapd_wifi.conf > hostap/hostapd/hostapd_wifi_amended.conf

wilo_kill_by_keyword hostapd_wifi_amended
 $WILO_SRCROOT/hostap/hostapd/hostapd -dd hostap/hostapd/hostapd_wifi_amended.conf
}

wilo_reload_supplicant() {
test ! 1 -eq $WILO_DEV_MODE || ( cd $WILO_SRCROOT/hostap/wpa_supplicant && cp defconfig .config && make -j8 ) || return 1
 killall wpa_supplicant
 $WILO_SRCROOT/hostap/wpa_supplicant/wpa_supplicant -dd -Dnl80211 -i $WLAN -c $WILO_SRCROOT/hostap/wpa_supplicant/wpa_supplicant_ad.conf
}

wilo_hostapd() {
 killall hostapd
 wilo_kill_by_keyword hostapd_11ad_amended
 /usr/sbin/hostapd -dd /home/wigig/work_ap/hostap___/hostapd/hostapd_11ad_amended.conf
}

wilo_get_ifname() {
    DRV="wil6210"

    for f in /sys/class/net/*; do {
        drv=`readlink $f/device/driver` || echo -n;
        drv=${drv##.*/}
        if [[ $drv == $DRV ]]; then {
                ifc=${f#/sys/class/net/}
                echo $ifc
                export WLAN=$ifc
                break
        } ; fi
    } ; done
    return 0
}

wilo_get_wifi_ifname() {
    wilo_get_ifname > /dev/null
    for i in /sys/class/net/* ; do {
        ifc=$(basename $i)
        if [ -d  "$i/phy80211" ] && [ x$WLAN != x$ifc ]; then {
            echo $ifc
            export WIFI=$ifc
            break
        } ; fi
    } ; done
}

wilo_reload_drv() {
echo "(1) STOPPING NETWORK MANAGER"
wilo_stop_nm
echo "(2) STOPPING FIREWALL SERVICE"
echo "(3) UNLOADING DRIVER"
wilo_remove
wilo_stop_firewall
sleep 1
echo "(4) LOADING DRIVER"
wilo_insert "$1"
sleep 5
wilo_get_ifname
dmesg | tail -20
 iw reg set "US"
echo "(5) LOADING DRIVER DONE"
}

wilo_reload_drv_pcp() {
echo "(1) STOPPING NETWORK MANAGER"
wilo_stop_nm
echo "(2) STOPPING FIREWALL SERVICE"
echo "(3) UNLOADING DRIVER"
wilo_remove
wilo_stop_firewall
sleep 1
echo "(4) LOADING DRIVER"
wilo_insert_pcp "$1"
sleep 5
wilo_get_ifname
dmesg | tail -20
 iw reg set "US"
echo "(5) LOADING DRIVER DONE"
}

wilo_load_sta() {
wilo_reload_drv "$1"
echo "(6) CONFIG INTERFACES WLAN=$WLAN"
 iw $WLAN set type managed
 ifconfig $WLAN down
#echo "(7) ifconfig $WLAN hw ether $STA_MAC_ADDR "
# ifconfig $WLAN hw ether $STA_MAC_ADDR
# ifconfig $WLAN $WILO_SUBNET.$IP_ADDR
 ifconfig $WLAN up 0.0.0.0
echo "(8) LOAD STA DONE"
}


wilo_load_sta_pcp() {
wilo_reload_drv_pcp "$1"
echo "(6) CONFIG INTERFACES WLAN=$WLAN"
 iw $WLAN set type managed
 ifconfig $WLAN down
#echo "(7) ifconfig $WLAN hw ether $STA_MAC_ADDR "
# ifconfig $WLAN hw ether $STA_MAC_ADDR
# ifconfig $WLAN $WILO_SUBNET.$IP_ADDR
 ifconfig $WLAN up 0.0.0.0
echo "(8) LOAD STA DONE"
}

wilo_load_sta_secure() {
wilo_load_sta secure_sta=1
}

wilo_load_supplicant() {
wilo_reload_drv
 iw $WLAN set type managed
echo ========================================
echo "(9) Loading STA $WLAN MAC=$STA_MAC_ADDR"
echo ========================================
 ifconfig $WLAN hw ether $STA_MAC_ADDR
 ifconfig $WLAN $WILO_SUBNET.2
wilo_reload_supplicant
}

wilo_scan() {
 iw $WLAN scan
}

wilo_connect() {
if [[ ! -n "$PEER" ]]
then
   PEER="$1"
fi
 iw $WLAN connect $PEER
}

wilo_wifi_connect()
{
 iw $1 scan
 iw $1 connect $2
}

wilo_wifi_disconnect()
{
 iw ${WIFI_IF} disconnect
}

wilo_disconnect() {
 iw $WLAN disconnect
}

wilo_pcp() {
wilo_reload_drv
 iw $WLAN set type __ap
 ifconfig $WLAN hw ether $AP_MAC_ADDR
 ifconfig $WLAN $WILO_SUBNET.1
 ifconfig $WLAN up
}

wilo_load_ap() {
SSID=$1

wilo_reload_drv
 ifconfig $WLAN hw ether $AP_MAC_ADDR
 ifconfig $WLAN $WILO_SUBNET.1
 ifconfig $WLAN up
wilo_reload_hostapd $SSID
}

wilo_sniffer() {
wilo_reload_drv
 iw $WLAN set type monitor
 iw $WLAN set monitor control
 ifconfig $WLAN up
}

wilo_start_tools() {
 $WILO_TOOLS_DIR/wlct_fltr_loader.sh start
}

wilo_stop_tools() {
 $WILO_TOOLS_DIR/wlct_fltr_loader.sh stop
}

wilo_burn_fw() {

echo "--- BURN FW $1 $2  ----"
BASE_DIR=/root/work/
FW_SUBDIR=$1
wilo_remove
wilo_stop_tools
if [[ -n "$WILO_IMAGES_LIST_DIR/images" ]]
then
  echo Burning FW: $BASE_DIR$FW_SUBDIR
  if [[ ! -d "$BASE_DIR$FW_SUBDIR" ]]
  then
    echo No FW found: $BASE_DIR$FW_SUBDIRR
    return 1
  fi
  rm -rf $WILO_IMAGES_LIST_DIR/images || return 2
  cp -r $BASE_DIR$FW_SUBDIR $WILO_IMAGES_LIST_DIR/images || return 3
fi
wilo_start_tools
wiburn_fw -wifi -$2
wilo_stop_tools
}

 wilo_ls_fw() {
  find $WILO_IMAGES_LIST_DIR/* -type d  | xargs -n1 basename | grep -v "^${WILO_IMAGES_SUBDIR}$"
 }

 #
 #echo "Reduce kernel traces.."
 #echo 'module wil6210 -p' > /sys/kernel/debug/dynamic_debug/control
