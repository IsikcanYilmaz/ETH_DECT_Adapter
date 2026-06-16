#!/usr/bin/env bash

IFACENAME1="enxcafe00000001"
IFACENAME2="enxcafe00000002"

IP1="111.222.111.2"
IP2="111.222.111.3"
GATEWAY="111.222.111.1"

IFACENAME=""
LOCAL_IP=""
REMOTE_IP=""

if [ "$(ip a | grep $IFACENAME1)" ]; then
  IFACENAME="$IFACENAME1"
  LOCAL_IP="$IP1"
  REMOTE_IP="$IP2"
elif [ "$(ip a | grep $IFACENAME2)" ]; then
  IFACENAME="$IFACENAME2"
  LOCAL_IP="$IP2"
  REMOTE_IP="$IP1"
fi

echo "Local Iface: $IFACENAME $LOCAL_IP"

sudo ip address flush dev $IFACENAME
sudo ip route flush dev $IFACENAME
sudo ip address add "$LOCAL_IP"/24 dev $IFACENAME
sudo ip route add "$REMOTE_IP" dev $IFACENAME

#
#
# sudo ip address add 111.222.111.2/24 brd + dev $IFACENAME1
# sudo ip address add 111.222.111.3/24 brd + dev $IFACENAME2
# # sudo ip route add 111.222.111.3 dev $IFACENAME # ping 111.222.111.3 and itll be routed thru CAFE00000001
# # sudo ip route add 111.222.111.3 dev $IFACENAME
# ip a show dev $IFACENAME
#
# # sudo ip address flush dev $IFACENAME 
# # sudo ip route flush dev $IFACENAME 
# # sudo ip address add 111.222.111.3/24 brd + dev $IFACENAME
# # sudo ip route add 111.222.111.1 dev $IFACENAME
# # ip a show dev $IFACENAME
