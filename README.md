# ESP32-S3-ETH + 4x Waveshare 96x48 HUB75 weather / image display

ESP-IDF **v5.5.4** application for the Waveshare ESP32-S3-ETH (onboard W5500) and four Waveshare RGB-Matrix-Px-96x48 panels arranged as a 192x96 2x2 U-chain.

The application is based on the attached Raspberry Pi `test.cc` example, rewritten for ESP-IDF / FreeRTOS. It does not use the Raspberry Pi RGB Matrix library, GraphicsMagick, `curl`, Linux files, or `/dev/i2c-*`.

## What appears on the display

- **Top 192x48 area:** horizontally scrolling image.
- **Bottom 192x48 area:**
  1. NTP date/time (India Standard Time) and OpenWeatherMap location.
  2. Weather description and wind speed.
  3. Temperature and relative humidity.
  4. OpenWeatherMap air-pollution category and PM2.5 concentration.

The screen is redrawn as one complete 192x96 frame, then the HUB75 driver's double buffer is flipped once. The image scroll offset is global to the full virtual canvas. HTTP requests are made from a separate FreeRTOS task and cannot block the animation loop. HUB75 DMA refresh continues independently.

## Panel layout: do not change this

This keeps the layout that was confirmed working on the user's physical installation:

```cpp
cfg.layout_rows = 2;
cfg.layout_cols = 2;
cfg.layout = Hub75PanelLayout::TOP_LEFT_DOWN;
```

Physical data chain:

```text
ESP32-S3 -> TOP-LEFT -> TOP-RIGHT
                            |
                            v
           BOTTOM-LEFT <- BOTTOM-RIGHT
```

The top pair is physically inverted and the bottom pair is upright. Do not add a second software 180-degree rotation. Do not change this to `TOP_LEFT_DOWN_ZIGZAG` unless the panel installation is physically changed.

## Project tree

```text
esp32s3_hub75_weather_image/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── idf_component.yml
│   ├── main.cpp
│   └── image.ppm
└── tools/
    └── convert_image.py
```

## 1. ESP-IDF environment

This project targets ESP-IDF v5.5.4. In a fresh terminal:

```bash
. "$HOME/esp/esp-idf-v5.5.4/export.sh"
idf.py --version
```

The version should report `ESP-IDF v5.5.4`.

## 2. Configure the image

A sample dusk landscape is provided as `main/image.ppm`. It is binary PPM (P6), RGB888, 384x48 pixels.

To use your own PNG/JPG:

```bash
python3 -m pip install --user Pillow
python3 tools/convert_image.py /path/to/your-photo.jpg main/image.ppm
```

The converter center-crops/resizes the image to 384x48. The firmware accepts P6 PPM with max value 255, width 192..2048 and height 48..256. The top 48 rows are displayed. A 384-pixel-wide image takes about 15.4 seconds to complete one scroll at the default one-pixel step and 40 ms frame period.

Because `main/image.ppm` is embedded into the firmware at build time, run `idf.py build` again after replacing it. No SD card or image filesystem is required at runtime.

## 3. Configure OpenWeatherMap

Create/enable an OpenWeatherMap API key for Current Weather and Air Pollution endpoints. Do not paste the key into `main.cpp` or commit it to Git.

Open menuconfig:

```bash
idf.py menuconfig
```

Navigate to **Weather display configuration** and set:

- `OpenWeatherMap API key`
- `Weather latitude` (default copied from the supplied Pi example: `22.317573`)
- `Weather longitude` (default copied from the supplied Pi example: `73.142594`)
- `Weather update interval (minutes)` (default 10)

The key is written to the generated local `sdkconfig` and compiled into firmware. Treat the firmware as containing that key; it is not a secure secret store. Keep `sdkconfig` out of source control.

## 4. Build and flash

From the project directory:

```bash
idf.py set-target esp32s3
idf.py reconfigure
idf.py menuconfig
idf.py build
```

Connect the ESP32-S3-ETH over USB and identify its serial port:

```bash
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

Flash and open the monitor (change the port if needed):

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Exit monitor with `Ctrl+]`.

## 5. W5500 Ethernet pins

The project initializes the board's onboard W5500 using the ESP-IDF 5.5.x SPI-Ethernet W5500 driver. It obtains an IPv4 address through DHCP.

| W5500 signal | ESP32-S3 GPIO |
|---|---:|
| MISO | 12 |
| MOSI | 11 |
| SCLK | 13 |
| CS | 14 |
| RESET | 9 |
| INT | 10 |

The W5500 reset line is pulsed during boot. SPI2 is set to 20 MHz. The default route is assigned to the Ethernet netif. `sdkconfig.defaults` enables the IDF SPI-Ethernet and W5500 options; no separate W5500 component is needed for this IDF version.

## 6. HUB75 GPIO map (same as the working demo)

| HUB75 signal | ESP32-S3 GPIO |
|---|---:|
| R1, G1, B1 | 1, 2, 15 |
| R2, G2, B2 | 16, 17, 18 |
| A, B, C, D, E | 38, 39, 40, 41, 42 |
| CLK | 21 |
| LAT | 47 |
| OE | 48 |

The display configuration stays at 96x48 per panel, standard two-scan row wiring, generic shift driver, 10 MHz clock, 90 Hz minimum refresh, double buffering enabled, brightness 80, and `TOP_LEFT_DOWN` layout.

Use the same external 5 V panel supply and common ground arrangement as your working display. Do not power four panels from the ESP32 board's 5 V pin. Verify the level shifter and wiring already used for your working setup.

## 7. Data semantics and Raspberry Pi-only parts

- Weather temperature and humidity come from OpenWeatherMap's current-weather endpoint; wind speed and description are also shown.
- AQI is OpenWeatherMap's **1..5 category** (`1 GOOD`, `2 FAIR`, `3 MODERATE`, `4 POOR`, `5 VERY POOR`), not India's CPCB numerical AQI scale (0..500). PM2.5 is shown separately in micrograms per cubic metre.
- The Pi example's `/home/pi/aqi_data.txt` cannot be opened by an ESP32. This port therefore displays the OpenWeatherMap AQI category and PM2.5 response. To show the old local sensor/file values too, expose that file from the Pi as an HTTP endpoint or provide the sensor's model and connection details; then a separate ESP32 input can be added.
- `fetchLocalTempData()` in the supplied C++ file was commented out at both call sites. Its HDC1080 Linux `/dev/i2c-1` implementation is not carried over. Weather temperature is the API temperature, as in the active path of the Pi demo. An HDC1080 can be added later using ESP-IDF's I2C master driver once the ESP32-S3 I2C GPIO pins are confirmed.
- Date/time is synchronized by SNTP and displayed in `Asia/Kolkata` offset (UTC+05:30).
- Initial API failures are retried after 60 seconds. After both weather and AQI refresh successfully, the configured interval (default 10 minutes) is used. On loss of Ethernet IP, the fetch task waits for the next IP event while the display continues.

## 8. Useful tuning constants

Near the top of `main/main.cpp`:

```cpp
constexpr int FRAME_DELAY_MS = 40; // 25 animation frames/s
constexpr int IMAGE_SCROLL_STEP = 1;
constexpr uint8_t DISPLAY_BRIGHTNESS = 80;
```

For faster image movement, increase `IMAGE_SCROLL_STEP` to 2. For slower movement, keep the step at 1 and increase `FRAME_DELAY_MS` to 50 or 60. Avoid changing the HUB75 pin map or panel layout when tuning animation speed.

## Notes

The source was adapted and reviewed against the ESP-IDF / HUB75 APIs, but this package has not been compiled or flashed on the user's physical ESP32-S3-ETH in this environment. The existing confirmed HUB75 layout and GPIO assignment are retained. If the Ethernet driver does not start, first verify the board is the Waveshare ESP32-S3-ETH revision with the W5500 pins listed above and inspect the serial error before changing GPIO assignments.
