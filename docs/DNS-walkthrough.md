Analyzed logs.txt:L1-134
Searched "getaddrinfo() returns 202" "esp-tls"
Canceled test OpenWeatherMap weather API URL
Killed task Test OpenWeatherMap weather API URL
Ran check DNS in sdkconfig
Searched "esp_netif_set_dns_info" "ESP_NETIF_DNS_MAIN" example
Analyzed main.cpp:L320-335
Edited main.cpp
Analyzed main.cpp:L320-355

### Summary: Why is the hostname not resolving?

| Question | Answer |
| :--- | :--- |
| **Is the API URL calling wrong?** | **No, the URL is 100% correct.** The format `https://api.openweathermap.org/data/2.5/weather?lat=...&lon=...` and `/air_pollution?...` exactly matches the official OpenWeatherMap API v2.5 documentation. |
| **Is the API key / API not correct?** | **No, the API server has not been reached yet.** In your log: `status=0`, `sock < 0`. Because the IP of the server could not be resolved, **no HTTP request was ever sent**, so the API key hasn't even been evaluated by OpenWeatherMap. |
| **Why is the hostname not getting resolved?** | **DNS failure on the local network.** The ESP32 cannot resolve *any* domain names to IP addresses. |

---

### Proof from your [logs.txt](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/logs.txt)

Notice two things in your log:
1. **SNTP also fails to resolve its server:**
   ```text
   W (15979) WEATHER_MATRIX: SNTP sync not completed yet: ESP_ERR_TIMEOUT
   ```
   SNTP is attempting to resolve `pool.ntp.org` and times out because DNS is unreachable.
2. **OpenWeatherMap hostname lookup fails before connecting:**
   ```text
   E (22979) esp-tls: couldn't get hostname for :api.openweathermap.org: getaddrinfo() returns 202, addrinfo=0x0
   E (22979) esp-tls: Failed to open new connection
   E (22979) transport_base: Failed to open a new connection
   E (22979) HTTP_CLIENT: Connection failed, sock < 0
   W (22989) WEATHER_MATRIX: HTTP request failed: err=ESP_ERR_HTTP_CONNECT status=0 overflow=0
   ```
   In lwIP / ESP-IDF, `getaddrinfo() returns 202` corresponds to `EAI_NONAME` / `HOST_NOT_FOUND` ("Name or service not known").

---

### Root Cause

Look at line 112 in your log:
```text
I (3979) esp_netif_handlers: eth ip: 192.168.2.2, mask: 255.255.255.0, gw: 192.168.2.4
```
Your ESP32 is assigned IP `192.168.2.2` with default gateway `192.168.2.4`. 

A gateway ending in `.4` indicates the ESP32 Ethernet cable is connected directly to your computer (or an ad-hoc adapter on `DSP-DEEP`), rather than a standard home router:

1. **Missing DNS Server in DHCP Lease**:
   When sharing an Ethernet connection from a PC or ad-hoc DHCP server, the DHCP server often does **not** provide DNS Server Option 6. Without a DNS server assigned, ESP-IDF defaults to `0.0.0.0`, causing `getaddrinfo()` to immediately fail with error `202`.
2. **PC Not Forwarding Internet / NAT**:
   If the ESP32 is connected directly to your Linux PC (`DSP-DEEP`), the PC must have **IP forwarding** and **NAT masquerade** enabled so the ESP32 can route packets out to the internet through your PC's Wi-Fi.

---

### Fix 1: Firmware Updated to Automatically Set Public DNS

[`main/main.cpp`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/main/main.cpp#L321-L351) has been updated to inspect the DHCP DNS server and automatically assign **Google DNS (`8.8.8.8`)** and **Cloudflare DNS (`1.1.1.1`)** when an IP is acquired:

```cpp
void ip_event_handler(...)
{
    ...
    esp_netif_dns_info_t dns_main{};
    esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    ESP_LOGI(TAG, "DHCP DNS Main: " IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));

    if (dns_main.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "DHCP provided no DNS server; configuring Google DNS (8.8.8.8)");
        dns_main.ip.type = ESP_IPADDR_TYPE_V4;
        dns_main.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
        esp_netif_set_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    }

    esp_netif_dns_info_t dns_backup{};
    dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    dns_backup.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
    esp_netif_set_dns_info(event->esp_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
}
```

---

### Fix 2: Check Network Sharing on Host PC (`192.168.2.4`)

If the ESP32 is connected directly to your Linux machine (`DSP-DEEP`):

1. **Enable "Shared to other computers" in NetworkManager (Ubuntu / Linux)**:
   - Go to **Settings** → **Network** → your **Wired / Ethernet** profile → **IPv4**.
   - Change IPv4 Method from *Automatic (DHCP)* or *Manual* to **"Shared to other computers"**.
   - This automatically enables DHCP, local DNS forwarding, and NAT masquerading so the ESP32 can access the internet through your PC's Wi-Fi connection.

2. **Or enable IP forwarding and NAT manually via terminal**:
   ```bash
   # Enable IPv4 forwarding
   sudo sysctl -w net.ipv4.ip_forward=1

   # Identify your internet-connected interface (e.g. wlan0 or wlp2s0) with `ip route`
   # Then enable NAT masquerade (replace wlan0 with your actual Wi-Fi interface):
   sudo iptables -t nat -A POSTROUTING -o wlan0 -j MASQUERADE
   ```

3. **Or plug the ESP32 into your router / switch** directly instead of the PC Ethernet port if your router has available ports.

---

### Next Step
Rebuild and flash:
```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
The monitor will log the assigned DNS and proceed to resolve `api.openweathermap.org` and fetch your live weather and air quality.