
#!/bin/sh

########################################################
# Wigig Release 17/10/2013
#
#Description:
# This script will upgrade your system to latest Wilocity components
# it was tested on Dell E62XX platforms and upgraded from Fedora18 
# kernel 3.8/3.9 to 3.10 branch kernel from kernel GIT + FST patch
#
#
#Components
#- FW based on FW TRUNK 3611
#- Wil6210 driver for Kernel 3.10
#- Wilocity Linux Tools for Kernel 3.10
#- Regulatory files for 60Ghtz
#- Hostapd  (from GIT+FST patch)
#- dhcpd  - DHCP Server
#- smbd  - Samba server
#- Wil6210 enable HW offloads TX/RX Checksum, Scatter Gatter, LSO (not default)
#- Wil6210 NAPI API support
#- Wil6210 Interrup coalesing

#Limitations:
#1- Traffic load balance is not optimized yet

 

##################################
#
#  Installation path is /root/work 
#  Make sure you have internet access !
#  Need ROOT permissions !!! (su)
#
##################################

WORK_DIR=/root/work
LINUX_INSTALL_DIR="/root/linux"
FW_IMAGE_DIR=/usr/local/bin/wilocity/burn_files/images/
WILO_TOOLS_DIR=/usr/local/bin/wilocity
PACKAGE_NAME=wigig_linux_package_1.0.8.tar.gz
LAST_SUPPORTED_KERNEL=3.10.0
OUT_DIR=OUT_3_10
GIT=`which git`
GCC=`which gcc`
SPARSE=`which sparse`
ETHTOOLS=`which ethtool`
BRCTL=`which brctl`
DHCPD=`which dhcpd`
SMBD=`which smbd`
PATCH=`which patch`
MAKE_CONFIG_CMD=oldconfig
LAST_COMPILED_KERNEL=$(uname -r)
BURN_FW_IMAGE=FW_3611
OLD_CONFIG=/lib/modules/$(uname -r)/build/.config
GIT_COMMIT_3_10=bc1b510b8979ecc322f8d930dde56658967b7355
WLCT_VER_3_10=wlt-2013-07-17-15-08-49-install.run
PATCH_KERNEL_3_10=linux_3_10_8a1c71c.diff
GIT_HOSTAPD_BASE=e112764e6d9e8154e3f33c667123143c3758e238
PATCH_HOSTAPD=hostap_fst_demo2_e112764_f0ddc76.diff

package_content() {
   cd $WORK_DIR
   #tar -zxvf {file.tar.gz}
   #unrar x WORK_DIR/$PACKAGE_NAME
}

#Kernel upgrade
#---------------
kernel_git_get() {
   cd /root 

   if [ -e "$ETHTOOLS" ] 
      then
      echo "Found ethtool in $ETHTOOLS "
   else
     echo "Installing ethtool: "
     yum install ethtool -y
   fi

   if [ -e "$GIT" ] 
      then
      echo "Found GIT in $GIT "
   else
     echo "Installing GIT: "
     yum install git -y
   fi

   if [ -e "$GCC" ] 
      then
      echo "Found GCC in $GCC "
   else
     echo "Installing GCC: "
     yum install gcc -y
   fi

   if [ -e "$SPARSE" ] 
      then
      echo "Found SPARSE in $SPARSE "
   else
     echo "Installing SPARSE: "
     yum install sparse -y
   fi

   if [ -e "$PATCH" ] 
      then
      echo "Found PATCH in $PATCH "
   else
     echo "Installing PATCH: "
     yum install patch -y
   fi

   if [ -d $LINUX_INSTALL_DIR ]
	then
		rm -rf $LINUX_INSTALL_DIR/OUT
		cd /root/linux
		echo "Now aligne with kernel 3.10 Release "
		git checkout master
		git pull
		git branch -D $LAST_SUPPORTED_KERNEL		
		git checkout -b $LAST_SUPPORTED_KERNEL $GIT_COMMIT_3_10
   else
   		echo "Clone latest kernel to /root/linux... this may take some time ..."
	   	git clone git://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git 
	   	echo "Now aligne with kernel 3.10 Release "
	   	cd /root/linux
		git checkout master
		git branch -D $LAST_SUPPORTED_KERNEL
	   	git checkout -b $LAST_SUPPORTED_KERNEL $GIT_COMMIT_3_10
  fi   

  git reset --hard
  git clean -f
  git status
  echo "Now apply daynix fst patch for kernel"
  patch -p1 -d /root/linux  < /root/work/$PATCH_KERNEL_3_10

}


test_old_config() {
   if [ -e "$OLD_CONFIG" ] 
      then
	LAST_COMPILED_KERNEL=$(uname -r)
	cp /lib/modules/$LAST_COMPILED_KERNEL/build/.config $WORK_DIR/kernel.config
      	echo "Found OLD CONFIG in kernel $LAST_COMPILED_KERNEL "
   else
     echo "Error OLD CONFIG -> $OLD_CONFIG not found "
     cd $WORK_DIR/linux
     yes "" | make config
     mv .config OUT/.config
     
   fi
}

kernel_make() {

   rm -rf /root/linux/$OUT_DIR
   echo "Compile kernel (using local .config kernel configuration) "
   cd /root/linux 
   make clean
   mkdir $OUT_DIR   

   # Unmark below call if kernel upgrade failed !!!  
   #test_old_config


   echo "Using fix kernel configuration from package upgrade"
   echo "==================================================="
   echo "==================================================="
   echo "If you have problem with upgrade kernel try use last"
   echo "Compiled/system kernel config:"
   echo "1- from /lib/modules/MYL_LAST_COMPILED_KERNEL/build/.config"
   echo "2- enable above test_old_config call."
   echo "==================================================="
   echo "==================================================="
 
   cp $WORK_DIR/kernel.config /root/linux/$OUT_DIR/.config
   
   echo "----------------------------------------------------------------------------"
   echo "Selecting default kernel configuration - oldconfig (/root/linux/OUT/.config)"
   echo "----------------------------------------------------------------------------"
   yes "" | make O=$OUT_DIR oldconfig
   
   make mrproper 

   cd $OUT_DIR 
   echo "------------------------------------------------------------"
   make -j4	 
}

kernel_install() {

   cd /root/linux/$OUT_DIR
   echo "Install new kernel: "
   make modules_install 
   make install 
}

#Regulatory update
#-------------------
update_utils() {

   echo "Copy regulatory files " 
   cd $WORK_DIR

   echo "Handle hostapd dependencies "
   yum install libnl-devel -y
   yum install openssl-devel -y   

   if [ -e "$BRCTL" ]
	then
		echo "Found brigs-utils in $BRCTL"
	else
		yum install bridge-utils -y
   fi

}

update_hostapd() {
   echo "Download hostapd from git "
   cd $WORK_DIR

   rm -rf hostap

   git clone git://w1.fi/srv/git/hostap.git
   cp config_hostapd hostap/hostapd/.config
   cp config_supplicant hostap/wpa_supplicant/.config
   
   cd hostap/
   echo "Align with [fst] base: $GIT_HOSTAPD_BASE"
   git checkout $GIT_HOSTAPD_BASE
   echo "Apply [fst] patch "
   patch -p1 < /root/work/$PATCH_HOSTAPD
   echo "Make hostapd"
   cd hostapd/
   make -j4
   sudo killall hostapd
   yes "y" | cp hostapd /sbin
   yes "y" | cp hostapd_cli /sbin

   echo "Make wpa_supplicant"
   cd ../wpa_supplicant
   make -j4

   # echo "Recompile hostapd with 11n HT support "
   
   # cd $WORK_DIR/hostap/hostapd/
   # yes "y" | rm .config
   # make clean
   # cp $WORK_DIR/hostapd_config.config $WORK_DIR/hostap/hostapd/.config
   # make -j4
   # cd $WORK_DIR
   # # Use next hostap configuration file for 11n 5 Ghtz channel 36
   # cp $WORK_DIR/hostapd_wifi.conf  $WORK_DIR/hostap/hostapd
}


#Regulatory update
#-------------------
update_regulatory_binaries() {

   echo "Copy regulatory files " 
   cd $WORK_DIR
   # just in case ...

   chmod +x wpa_supplicant wpa_cli crda regdbdump

   cp wpa_supplicant wpa_cli crda regdbdump /sbin/ 
   cp regulatory.bin /lib/crda/ 
   cp vkondrat.key.pub.pem /lib/crda/pubkeys/ 
   echo "Copy done"

}

compile_driver() {
   echo "Compile new Wil6210 driver"

   cd $WORK_DIR/wil6210
   chmod +x *.sh tools/* wil6210_start 
   make CONFIG_WIL6210=m 
}

compile_wilo_tools() {
  chmod +x $WORK_DIR/$WLCT_VER_3_10
  echo "First try to uninstall Wilocity Tools"
  sudo $WORK_DIR/$WLCT_VER_3_10 uninstall
  echo "Install Wilocity Tools: $WLCT_VER_3_10"
  sudo $WORK_DIR/$WLCT_VER_3_10
}


upgrade_kernel() {

   if [ "$LAST_COMPILED_KERNEL" == "$LAST_SUPPORTED_KERNEL" ] ; then
	echo " Skip kernel upgrade -> already in $LAST_COMPILED_KERNEL"

	#update_regulatory_binaries
	return;	
   fi

  kernel_git_get
  kernel_make
  kernel_install
  echo "Now Reboot and continue with Driver compilation"
  #reboot
}

upgrade_driver() {
  update_regulatory_binaries
  update_utils
  update_hostapd
  compile_driver
  compile_wilo_tools
}

update_product_platform_ini() {
   echo "Replace Fei-Dai Board INI "
   cp $WORK_DIR/$BURN_FW_IMAGE/Hitachi3p1_Dell_6430u.ini $WORK_DIR/$BURN_FW_IMAGE/LTCC_generic.ini
}

fw_upgrade_feidao() {
  echo "Burn Fei-Dao platform "
  yes "y" | cp $BURN_FW_IMAGE/* $FW_IMAGE_DIR
  sudo $WILO_TOOLS_DIR/wlct_fltr_loader.sh start
  yes "0" | wiburn_fw -wifi -feidao
}

fw_upgrade_ltcc() {
  echo "Burn LTCC platform "
  yes "y" | cp $BURN_FW_IMAGE/* $FW_IMAGE_DIR
  sudo $WILO_TOOLS_DIR/wlct_fltr_loader.sh start
  yes "0" | wiburn_fw -wifi -ltcc
}

upgrade_fw() {

  echo " Burining new FW from $2, platform is $1 "
  
  sudo $WILO_TOOLS_DIR/wlct_fltr_loader.sh stop

  cd $WORK_DIR

  case $1 in
     -fei-dao) fw_upgrade_feidao ;;
     -ltcc) fw_upgrade_ltcc ;;
	*) echo "ERROR $1, $2 "
  esac

}


dhcp_server_install() {
   # dhcp server install
   if [ -e "$DHCPD" ]
	then
		echo "Found dhcpd server in $DHCPD"
	else
		yum install dhcpd -y
		echo "Copy DHCPD config file to /etc/dhcp/ "
		cp $WORK_DIR/dhcpd.conf /etc/dhcp/ 
   fi

}


samba_server_install () {

   # samba server install
   if [ -e "$SMBD" ]
	then
		echo "Found smbd server in $SMBD"
	else
		yum install samba -y
		echo "Copy SMBD config file to /etc/samba/ "
		cp $WORK_DIR/smbd.conf /etc/samba/ 
   fi
}


auto_run_ap() {
	if [ -e /etc/init.d/ap_service ]
	then
		echo "Already support AP autorun service, add FST capabilities"
		#return;
	fi

	echo "Add AP service to startup "	
	cd $WORK_DIR
	chmod +x ap_service
	cp ap_service /etc/init.d/
	ln -s /etc/rc2.d/S90ap_service /etc/init.d/ap_service
	ln -s /etc/rc3.d/S90ap_service /etc/init.d/ap_service
	ln -s /etc/rc4.d/S90ap_service /etc/init.d/ap_service
	ln -s /etc/rc5.d/S90ap_service /etc/init.d/ap_service
}

echo "Upgrade process started: "
echo "Running upgrade - $1 $2 $3"
echo "You are about to upgrade you platform to Wilocity Wigig release 2.0.2 "
echo "The flowing components will be upgrade: "
echo " kernel - Kernel (3.10)"
echo " driver - HOSTAPD (GIT), Wilocity Tools (2013-07-18), Wilocity Wigig driver wil6210"
echo " fw - Wilocity FW"
echo " dhcpd - DHCP Server - for AP"
echo " smbd - SAMBA Server - for AP"
echo " ap_start- Auto run AP - for AP"

echo $1 $2 
case $1 in
  kernel) upgrade_kernel ;;
  driver) upgrade_driver ;;
  fw) upgrade_fw $2 $3 ;;
  dhcpd) dhcp_server_install ;;
  smbd) samba_server_install ;;
  ap_start) auto_run_ap ;;
  *) { echo " Unknown command <$1> use- kernel, driver, fw, dhcpd, smbd server or ap_start" exit 1; } ;;
esac




