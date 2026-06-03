#!/usr/bin/env bash

IFACENAME1="enxcafe00000001"
IFACENAME2="enxcafe00000002"

sudo ip address flush dev $IFACENAME1 
sudo ip route flush dev $IFACENAME1

sudo ip address flush dev $IFACENAME2
sudo ip route flush dev $IFACENAME2

sudo ip address add 111.222.111.2/24 brd + dev $IFACENAME1
sudo ip address add 111.222.111.3/24 brd + dev $IFACENAME2
# sudo ip route add 111.222.111.3 dev $IFACENAME # ping 111.222.111.3 and itll be routed thru CAFE00000001
# sudo ip route add 111.222.111.3 dev $IFACENAME
ip a show dev $IFACENAME

# sudo ip address flush dev $IFACENAME 
# sudo ip route flush dev $IFACENAME 
# sudo ip address add 111.222.111.3/24 brd + dev $IFACENAME
# sudo ip route add 111.222.111.1 dev $IFACENAME
# ip a show dev $IFACENAME
