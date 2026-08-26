# Jon's DECT NR+ notes
- nRF connect to flash the board(s) with basic firmware. 
- pyTerm to get shell.
- use the nrfutil sdk-manager commands to install a version of the sdk. use search to see what's avail
- 
- Use `launch_venv.sh` to launch the virutal environment of nRF's toolchain.
- Sample code resides in `sdk-nrf/samples/dect/dect_phy/`
- Go into it. run (while )

# Getting up and running fresh
- Download `nrfutil` and ln -s it into /usr/bin/
- `nrfutil install sdk-manager`
- `sudo apt install west`
- `sudo dpkg install JLink_Linux_x86_64.deb`

# Commands of interest
- `nrfutil sdk-manager search`
- `nrfutil sdk-manager install v3.2.0-rc2`
- `nrfutil sdk-manager toolchain launch --ncs-version v3.2.0-rc2 --shell`
- build command `west build -p -b nrf9161dk/nrf9161/ns -- -DEXTRA_CONF_FILE=overlay-eu.conf` at least this builds the hello example code. adding `flash` to this flashes

# Workflow:
- Lets say it's been a while since you looked at this stuff. or youre on a fresh machine. you sat down you want to do DECT NR + work. you do the following:
- `./launch_venv.sh`  # this assumes you have an sdk installed. you may not. if you dont, go thru https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html
- `nrfutil install sdk-manager`
- `nrfutil sdk-manager search` # shows available sdk versions
- `nrfutil sdk-manager install v3.3.0` # this will take a bit if we need to download
- After installing this run `launch_venv.sh`
- `west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.2.0-rc2 ./sdk-nrf` # initializes west, zephyr's build utility, against v3.2.0-rc2 version nrf sdk, puts it into the current dir you are in, names it sdk-nrf. you need to have run `launch_venv.sh`.
- go into this dir and run `west update` this will get the submodules
- at this point you should have in your environment a working nrfutil and an sdk
- go to, for example, sdk-nrf/nrf/samples/dect/dect_phy
- build command `west build -p -b nrf9161dk/nrf9161/ns -- -DEXTRA_CONF_FILE=overlay-eu.conf` at least this builds the hello example code
- kconfig gui command `west build -p -b nrf9151dk/nrf9151/ns -t menuconfig`
- flash command `west flash`

# Hints
- Board files at /home/jon/KODMOD/DECTNRPLUS/sdk-nrf/zephyr/boards/nordic/nrf9161dk/
- We are currently using this `Non secure` version/mode thing. i dont exactly know what this entails but it means we're using thje `_ns` board files.
- ~I had to modify /home/jon/KODMOD/DECTNRPLUS/sdk-nrf/zephyr/boards/nordic/nrf9161dk/nrf9161dk_nrf9161_ns.dts to enable UART1~
- To enable/disable/config device tree devices you need a `.overlay` file for your project to override default configs. To include your overlay file in the compilation add `-DDTC_OVERLAY_FILE=x.overlay` in your build command or add it to your CMakeLists.txt
- For example, to enable uart1 put the following in a overlay file
`
&uart1 {
    status="okay";
};
`
- To modify the default configs of devices, write your own `.overlay` file like mentioned above
- To view the hardware configuration after compilation, find the `zephyr.dts` file in the build/ directory

# Flashing the modem binary
- in modem_bins i have stefans modem firmwares
- `nrfjprog --ids` to get devices and serial numbers
- `nrfutil device list` also does this but better
- `udevadm info /dev/ttyACM0 | grep -E 'SERIAL|ID_PATH'` to see which serial is at which tty
- `nrfjprog --program mfw-nr+_nrf91x1_1.1.0.zip --snr 1051262705 -f NRF91 --reset --verify` this is the jlink way
- a better way is `nrfutil device program --firmware <fw path> --serial-number <number>`

# Links of interest
- nRF Connect SDK: https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html
- nRF Util: https://docs.nordicsemi.com/bundle/nrfutil/page/guides/installing.html and https://www.nordicsemi.com/Products/Development-tools/nRF-Util/Download#infotabs
- SDK docs: https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/app_dev/create_application.html
- PHY Hello: https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/dect/dect_phy/hello_dect/README.html
- Arduino ETH shield: https://docs.wiznet.io/Product/Chip/Ethernet/W5500/W5500-Ethernet-Shield/w5500_ethernet_shield
- nrfxlib DECT docs: https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrfxlib/nrf_modem/doc/dect/dectphy.html#scheduling-operations

# MAC Layer release from nRF
- In 2026 nRF released a modem firmware that has MAC functionality. They give it to you if you ask the marketing people.
- Here, its located in modem_bin/mfw-nr+_nrf91x1_2.0.0.zip
- Flash it using nrfconnect
- `https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/hands-on-with-dect-nr-api-release-v2-0`
- `https://github.com/lauri-piikivi/dect-nr-samples.git`

# Loose notes
- build command `west build -p -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-eu.conf -DDTC_OVERLAY_FILE=dongle.overlay`

