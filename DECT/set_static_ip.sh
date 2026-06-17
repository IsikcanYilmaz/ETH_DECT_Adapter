#!/usr/bin/env bash

IFACENAME1="enxcafe00000001"
IFACENAME2="enxcafe00000002"
MAC1="ca:fe:00:00:00:01"
MAC2="ca:fe:00:00:00:02"
IP1="111.222.111.1"
IP2="111.222.111.2"

sudo ip address flush dev $IFACENAME1
sudo ip route flush dev $IFACENAME1
sudo ip address flush dev $IFACENAME2
sudo ip route flush dev $IFACENAME2

sudo ip address add $IP1/32 brd + dev $IFACENAME1
sudo ip address add $IP2/32 brd + dev $IFACENAME2

sudo ip route add $IP2 dev $IFACENAME1
sudo ip route add $IP1 dev $IFACENAME2

sudo arp -s $IP1 $MAC1 -i $IFACENAME1
sudo arp -s $IP2 $MAC2 -i $IFACENAME2

ip a show dev $IFACENAME1
ip a show dev $IFACENAME2
