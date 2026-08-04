#!/usr/bin/env bash

IP1="111.222.111.2"
IP2="111.222.111.3"

IP1="10.42.0.1"
IP2="10.42.0.2"
ROVER_IP="10.42.0.50"

GATEWAY="111.222.111.1"

LOCAL_IP=""
REMOTE_IP=""

FLEET_MANAGER_IFACE_NAME="enp4s0"
TOBIAS_IFACE_NAME="enp6s0"
THINKPAD_IFACE_NAME="enp1s0f0"
OFFICE_IFACE_NAME="enxa0cec81dcbce"
ROVER_IFACE_NAME="eth0"

TOBIAS_MAC="84:a9:38:58:59:9d"
THINKPAD_MAC="74:5d:22:8d:74:0d"
OFFICE_MAC="a0:ce:c8:1d:cb:ce"
ROVER_MAC="d8:3a:dd:9a:05:d7"
FLEET_MANAGER_MAC="f0:b2:b9:11:ca:fa"

if [ "$(hostname)" == "jon-ThinkPad-T14-Gen-4" ]; then
  echo "[+] I am THINKPAD"
  LOCAL_IP="$IP1"
  REMOTE_IP="$IP2"
  IFACENAME="enp1s0f0"
  # LOCAL_MAC="74:5d:22:8d:74:0d"
  # REMOTE_MAC="a0:ce:c8:1d:cb:ce"
  LOCAL_MAC="$THINKPAD_MAC"
  REMOTE_MAC="$TOBIAS_MAC"
elif [ "$(hostname)" == "agv" ]; then
  echo "[+] I am ROVER"
  LOCAL_IP="$ROVER_IP"
  REMOTE_IP="$IP1"
  IFACENAME="eth0"
  LOCAL_MAC="$ROVER_MAC"
  REMOTE_MAC="$FLEET_MANAGER_MAC"
elif [ "$(hostname)" == "agv-fleet-manager" ]; then
  ehco "[+] I am FLEET MANAGER"
  LOCAL_IP="10.42.0.1"
  REMOTE_IP="$ROVER_IP"
  IFACENAME="$FLEET_MANAGER_IFACE_NAME"
  LOCAL_MAC="$FLEET_MANAGER_MAC"
  REMOTE_MAC="$ROVER_MAC"
else
  echo "[+] I am OFFICE"
  LOCAL_IP="$IP2"
  REMOTE_IP="$IP1"
  IFACENAME="enxa0cec81dcbce"
  # LOCAL_MAC="a0:ce:c8:1d:cb:ce"
  # REMOTE_MAC="74:5d:22:8d:74:0d"
  LOCAL_MAC="$TOBIAS_MAC"
  REMOTE_MAC="$THINKPAD_MAC"
fi

echo "Local: $LOCAL_IP" "$LOCAL_MAC" $IFACENAME
echo "Remote: $REMOTE_IP" "$REMOTE_MAC"

sudo ip address flush dev $IFACENAME
sudo ip route flush dev $IFACENAME
sudo ip address add "$LOCAL_IP"/24 dev $IFACENAME
sudo ip route add "$REMOTE_IP" dev $IFACENAME
sudo ip neigh add "$REMOTE_IP" lladdr "$REMOTE_MAC" dev "$IFACENAME"
echo sudo ip neigh add "$REMOTE_IP" lladdr "$REMOTE_MAC" dev "$IFACENAME"

