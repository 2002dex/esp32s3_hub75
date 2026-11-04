Searched "192.168.2.4" DHCP gateway
Ran run ipconfig on Windows host
Searched "esp_netif" get dhcp server ip
Searched "etharp_find_addr" lwip esp-idf
Searched "esp_netif_set_ip_info" IP_EVENT_ETH_GOT_IP
Analyzed Kconfig.projbuild:L1-27
Searched "netif_default" esp-idf
Analyzed Kconfig.projbuild:L1-27
Edited Kconfig.projbuild
Analyzed main.cpp:L280-440
Edited main.cpp
Analyzed main.cpp:L430-460
Edited main.cpp
Analyzed sdkconfig.defaults:L1-7
Edited sdkconfig.defaults
Edited sdkconfig
Analyzed main.cpp:L430-480

### 1. Why is the ESP32 getting `192.168.2.2` with gateway `192.168.2.4`?

DHCP uses network-wide broadcast (`255.255.255.255`). When the ESP32 boots up and asks for an IP address over the 16-port switch, **any device running a DHCP server on that physical switch can respond**. 

DHCP operates on a **first-come, first-served race condition**:
1. The ESP32 broadcasts `DHCPDISCOVER`.
2. A device with IP **`192.168.2.4`** on your 16-port switch responded with a `DHCPOFFER` faster than your main router (`192.168.1.1`), or the switch port the ESP32 is plugged into is segmented.
3. The ESP32 accepted the lease: IP `192.168.2.2`, gateway `192.168.2.4`.
4. Because `192.168.2.4` has no internet routing or DNS forwarder, all internet requests (`api.openweathermap.org`, `pool.ntp.org`) immediately fail with `getaddrinfo() returns 202`.

---

### 2. Common culprits on a 16-port switch

What else is plugged into that 16-port switch?
- **A second Wi-Fi router / Access Point / Range Extender**: Often plugged into the switch via a LAN port with its internal DHCP server left enabled on `192.168.2.x`.
- **CCTV NVR / DVR / IP Camera box**: Many NVRs or PoE camera switches run a DHCP server on `192.168.2.x`.
- **Managed Switch with DHCP Server or VLAN**: If the 16-port switch is a "Smart/Managed" switch, it may have VLANs or an internal DHCP server enabled.
- **PLC / Industrial device / Raspberry Pi / NAS**: Any server or controller configured to provide IP addresses.

---

### 3. How to debug who `192.168.2.4` is

#### Option A: Find its MAC address and web page
From your laptop, add a secondary IP in the `192.168.2.x` range to talk to it directly:

1. Open **Command Prompt as Administrator** on your Windows laptop:
   ```cmd
   netsh interface ip add address "WiFi" 192.168.2.100 255.255.255.0
   ```
2. Ping the rogue gateway and check its MAC address:
   ```cmd
   ping 192.168.2.4
   arp -a | findstr "192.168.2.4"
   ```
3. Open `http://192.168.2.4` or `https://192.168.2.4` in your browser. The login screen will reveal whether it is a router, camera NVR, access point, or switch.
4. When finished, remove the secondary IP:
   ```cmd
   netsh interface ip delete address "WiFi" 192.168.2.100
   ```

#### Option B: Switch port test
- Check which port connects to your main router (`192.168.1.1`).
- Temporarily plug the ESP32 into a port right next to the router's uplink cable, or unplug suspect devices one by one to see when `192.168.1.x` is offered.

---

### 4. Immediate Solution: Static IP (Bypass the Rogue DHCP Server)

You do not need to wait to identify the rogue device. By assigning a **Static IP** directly on your router's subnet (`192.168.1.x`), the ESP32 will **completely ignore all DHCP traffic** and communicate directly with your router at `192.168.1.1`.

#### Changes made in the project:
1. **[`main/Kconfig.projbuild`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/main/Kconfig.projbuild)**: Added an *Ethernet network configuration* menu.
2. **[`main/main.cpp`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/main/main.cpp#L448-L470)**: When static IP is enabled, it stops the DHCP client, assigns the static IP, and configures the default gateway and DNS to your router.
3. **[`sdkconfig`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/sdkconfig)** & **[`sdkconfig.defaults`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/sdkconfig.defaults)**: Enabled Static IP with these settings:
   - **Static IP**: `192.168.1.222`
   - **Subnet Mask**: `255.255.255.0`
   - **Gateway**: `192.168.1.1`
   - **Primary DNS**: `192.168.1.1`
   - **Backup DNS**: `8.8.8.8`

---

### 5. Flash and Verify

In WSL2:
```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

**What you will see in the monitor:**
- `Configuring Static IP: 192.168.1.222, Netmask: 255.255.255.0, Gateway: 192.168.1.1`
- `W5500 Ethernet link connected`
- `Static IP active: 192.168.1.222, Gateway: 192.168.1.1`
- `SNTP time synchronized`
- `Weather updated: ... C, ...% humidity`
- `AQI updated: ...`

You can also ping `192.168.1.222` from your laptop (`192.168.1.12`) to verify bidirectional communication.