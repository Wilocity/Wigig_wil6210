sysctl -w net.ipv4.tcp_wmem="32768 1398080 33554432"
sysctl -w net.ipv4.tcp_rmem="32768 1398080 33554432"
sysctl -w net.core.wmem_max=33554432
sysctl -w net.core.rmem_max=33554432
sysctl -w net.ipv4.ipfrag_low_thresh=262144
sysctl -w net.ipv4.ipfrag_high_thresh=393216
ifconfig wlan1 txqueuelen 2000
sysctl -w net.ipv4.tcp_sack=0
sysctl -w net.ipv4.tcp_dsack=0
