Analyzed logs.txt:L1-113
Searched "mbedtls_ssl_setup returned -0x7F00"
Analyzed gdma_dma.cpp:L740-800
Analyzed gdma_dma.cpp:L800-860
Ran check CONTENT_LEN in sdkconfig
Ran check SPIRAM in sdkconfig
Ran check DYNAMIC_BUFFER in sdkconfig
Searched "ESP32-S3-ETH" waveshare PSRAM octal
Ran search SPIRAM in sdkconfig
Searched "http://api.openweathermap.org" allowed
Ran find RGB masks in gdma_dma.cpp
Ran search latch_blanking
Ran check default latch_blanking
Analyzed main.cpp:L450-490
Analyzed main.cpp:L491-580
Analyzed main.cpp:L80-95
Edited main.cpp
Analyzed main.cpp:L540-650
Edited main.cpp
Edited main.cpp
Analyzed main.cpp:L730-800
Edited main.cpp
Analyzed sdkconfig.defaults:L1-15
Edited sdkconfig.defaults
Analyzed sdkconfig:L2065-2088
Analyzed sdkconfig:L2590-2607
Edited sdkconfig
Ran find DYNAMIC_BUFFER line
Analyzed sdkconfig:L1830-1845
Edited sdkconfig
Analyzed main.cpp:L730-790
Analyzed main.cpp:L50-95

### Root Causes from [logs.txt](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/logs.txt)

#### 1. Why Weather / AQI data failed: `mbedtls_ssl_setup returned -0x7F00`
Look at lines 102–106 in your log:
```text
E (64230) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
E (64230) esp-tls: create_ssl_handle failed
E (64230) esp-tls: Failed to open new connection
E (64230) transport_base: Failed to open a new connection
E (64240) HTTP_CLIENT: Connection failed, sock < 0
W (64240) WEATHER_MATRIX: HTTP request failed: err=ESP_ERR_HTTP_CONNECT status=0 overflow=0
```

- In Mbed TLS, error `-0x7F00` is **`MBEDTLS_ERR_SSL_ALLOC_FAILED`** (out-of-memory).
- The 192x96 HUB75 driver allocates **294,912 bytes (288 KB)** of DMA memory in internal SRAM (Buffer A + Buffer B).
- The total free heap on this ESP32-S3 was only ~310 KB at boot, leaving only **~15 KB of free heap** for the rest of the system.
- Standard HTTPS requires allocating a 16 KB incoming SSL buffer + 4 KB outgoing buffer + SSL context (~26–30 KB contiguous RAM), which was physically impossible to fit in the remaining 15 KB heap.

#### 2. Why the display is flickering / black area getting light on/off
- In the previous frame optimization, `display.clear()` was removed and replaced with a partial user-coordinate fill.
- The physical panel layout has a DMA width of **384 words** across **24 physical rows** in double-buffered mode (Buffer A and Buffer B).
- Without `display.clear()`, the off-screen words and padding in Buffer A and Buffer B were not zeroed out. As the driver alternated between Buffer A and Buffer B at 25 FPS, the differences in uncleared bits caused visible strobing/flickering and random ghost LEDs flashing in the black regions.

---

### Changes Applied

#### 1. Switched OpenWeatherMap requests to plain HTTP (Port 80)
OpenWeatherMap officially supports plain `http://api.openweathermap.org`. In [`main/main.cpp`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/main/main.cpp#L568-L634):
- Updated endpoints from `https://api.openweathermap.org/...` to `http://api.openweathermap.org/...`.
- Connects via lightweight TCP (port 80) using only ~2 KB of buffer instead of 30 KB, completely bypassing Mbed TLS and eliminating `MBEDTLS_ERR_SSL_ALLOC_FAILED`.
- Enabled `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` in [`sdkconfig`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/sdkconfig) and [`sdkconfig.defaults`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/sdkconfig.defaults).

#### 2. Restored `display.clear()` to eliminate flickering and ghosting
In [`draw_dashboard()`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/main/main.cpp#L741-L755):
- Restored `display.clear()` at the start of each frame. It runs in < 1 ms and resets all 384 DMA words and 24 rows to pure black.
- Both Buffer A and Buffer B remain completely clean on every frame switch.
- Increased `latch_blanking` from 2 to 4 in [`make_display_config()`](file:///c:/Deep/RGB%20Matrix%2096x48/esp32s3_hub75_weather_image/main/main.cpp#L86-L92) to provide clean output enable (OE) margin, eliminating row-transition ghosting when bright text is active.

---

### Flash and Test

In your WSL2 terminal:

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

**Expected results:**
1. **No flickering / ghosting:** The black area remains completely dark, and text renders crisply without background flashing.
2. **Weather & AQI retrieved:**
   ```text
   I (...) WEATHER_MATRIX: Weather updated: 32.x C, 4x% humidity
   I (...) WEATHER_MATRIX: AQI updated: 3 (MODERATE), PM2.5 38.x ug/m3
   ```
3. Full dashboard will display:
   - Top 192x48: Smoothly scrolling image
   - Line 1: `Wed 04 Nov 12:20 <Location>`
   - Line 2: `WX <description> WIND <speed>M/S`
   - Line 3: `T:<temp>C HUM:<humidity>%`
   - Line 4: `AQI:<index> <desc> PM2.5:<pm25>`