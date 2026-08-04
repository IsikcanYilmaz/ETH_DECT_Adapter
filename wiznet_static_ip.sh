#!/usr/bin/env bash

IP1="111.222.111.2"
IP2="111.222.111.3"

IP1="10.42.0.1"
IP2="10.42.0.2"

GATEWAY="111.222.111.1"

LOCAL_IP=""
REMOTE_IP=""

if [ "$(hostname)" == "jon-ThinkPad-T14-Gen-4" ]; then
  echo "[+] I am THINKPAD"
  LOCAL_IP="$IP2"
  REMOTE_IP="$IP1"
  IFACENAME="enp1s0f0"
  LOCAL_MAC="74:5d:22:8d:74:0d"
  REMOTE_MAC="a0:ce:c8:1d:cb:ce"
else
  echo "[+] I am OFFICE"
  LOCAL_IP="$IP1"
  REMOTE_IP="$IP2"
  IFACENAME="enxa0cec81dcbce"
  LOCAL_MAC="a0:ce:c8:1d:cb:ce"
  REMOTE_MAC="74:5d:22:8d:74:0d"
fi

echo "Local: $LOCAL_IP"
echo "Remote: $REMOTE_IP"

sudo ip address flush dev $IFACENAME
sudo ip route flush dev $IFACENAME
sudo ip address add "$LOCAL_IP"/24 dev $IFACENAME
sudo ip route add "$REMOTE_IP" dev $IFACENAME
sudo ip neigh add "$REMOTE_IP" lladdr "$REMOTE_MAC" dev "$IFACENAME"
echo sudo ip neigh add "$REMOTE_IP" lladdr "$REMOTE_MAC" dev "$IFACENAME"

