#include "sdkconfig.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include "hub75.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" {
extern const uint8_t image_ppm_start[] asm("_binary_image_ppm_start");
extern const uint8_t image_ppm_end[] asm("_binary_image_ppm_end");
}

namespace {

constexpr char TAG[] = "WEATHER_MATRIX";

// ------------------------------ Display ----------------------------------
constexpr int PANEL_WIDTH = 96;
constexpr int PANEL_HEIGHT = 48;
constexpr int LAYOUT_COLS = 2;
constexpr int LAYOUT_ROWS = 2;
constexpr int DISPLAY_WIDTH = PANEL_WIDTH * LAYOUT_COLS;    // 192
constexpr int DISPLAY_HEIGHT = PANEL_HEIGHT * LAYOUT_ROWS; // 96
constexpr int IMAGE_VIEW_HEIGHT = 48;
constexpr int FRAME_DELAY_MS = 40; // 25 animation frames/second
constexpr int IMAGE_SCROLL_STEP = 1;
constexpr uint8_t DISPLAY_BRIGHTNESS = 80;

Hub75Config make_display_config()
{
    Hub75Config cfg{};
    cfg.panel_width = PANEL_WIDTH;
    cfg.panel_height = PANEL_HEIGHT;
    cfg.scan_wiring = Hub75ScanWiring::STANDARD_TWO_SCAN;
    cfg.shift_driver = Hub75ShiftDriver::GENERIC;

    // Keep the exact layout that was confirmed working on this installation:
    // controller -> top-left -> top-right -> bottom-right -> bottom-left.
    // Top pair is physically inverted; bottom pair is upright.
    cfg.layout_rows = LAYOUT_ROWS;
    cfg.layout_cols = LAYOUT_COLS;
    cfg.layout = Hub75PanelLayout::TOP_LEFT_DOWN;

    // Existing ESP32-S3-ETH HUB75 pin map. Do not change for this application.
    cfg.pins.r1 = 1;
    cfg.pins.g1 = 2;
    cfg.pins.b1 = 15;
    cfg.pins.r2 = 16;
    cfg.pins.g2 = 17;
    cfg.pins.b2 = 18;
    cfg.pins.a = 38;
    cfg.pins.b = 39;
    cfg.pins.c = 40;
    cfg.pins.d = 41;
    cfg.pins.e = 42;
    cfg.pins.lat = 47;
    cfg.pins.oe = 48;
    cfg.pins.clk = 21;

    // Signal integrity and anti-ghosting tuning:
    // 10 MHz clock and 90 Hz refresh rate provide rock-solid, vibration-free display.
    // clk_phase_inverted must be false so rising clock edges occur mid-bit (data stable),
    // eliminating letter jitter/vibration. Latch blanking = 4 prevents row ghosting.
    cfg.output_clock_speed = Hub75ClockSpeed::HZ_10M;
    cfg.min_refresh_rate = 90;
    cfg.latch_blanking = 4;
    cfg.clk_phase_inverted = false;
    cfg.double_buffer = true;
    cfg.brightness = DISPLAY_BRIGHTNESS;
    return cfg;
}

// Compact 5x7 ASCII font: uppercase letters, digits and punctuation used by
// the dashboard. Each value is one row; five low bits are pixels, left first.
struct Glyph {
    char ch;
    uint8_t row[7];
};

static constexpr Glyph FONT[] = {
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0F,0x10,0x10,0x10,0x10,0x10,0x0F}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0F,0x10,0x10,0x13,0x11,0x11,0x0F}},
    {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J',{0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
    {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
    {':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'%',{0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}},
    {'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {',',{0x00,0x00,0x00,0x00,0x04,0x04,0x08}},
    {'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'?',{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {' ',{0,0,0,0,0,0,0}},
};

void draw_text(Hub75Driver &display, const char *text, int x, int y,
               uint8_t red, uint8_t green, uint8_t blue)
{
    if (text == nullptr) return;
    int cursor_x = x;
    for (const char *p = text; *p != '\0' && cursor_x < DISPLAY_WIDTH; ++p) {
        const uint8_t *rows = nullptr;
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
        for (const auto &glyph : FONT) {
            if (glyph.ch == c) { rows = glyph.row; break; }
        }
        if (rows == nullptr) {
            for (const auto &glyph : FONT) {
                if (glyph.ch == '?') { rows = glyph.row; break; }
            }
        }
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((rows[row] & (1U << (4 - col))) == 0) continue;
                const int px = cursor_x + col;
                const int py = y + row;
                if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                    display.set_pixel(static_cast<uint16_t>(px), static_cast<uint16_t>(py),
                                      red, green, blue);
                }
            }
        }
        cursor_x += 6;
    }
}

// ------------------------- Embedded PPM image -----------------------------
// The PPM payload remains memory-mapped in flash; no 55 KB RAM copy is made.
struct PpmImage {
    int width = 0;
    int height = 0;
    const uint8_t *rgb_data = nullptr;

    bool valid() const { return width > 0 && height > 0 && rgb_data != nullptr; }

    static bool next_token(const uint8_t *&p, const uint8_t *end,
                           char *out, size_t out_size)
    {
        while (p < end) {
            if (std::isspace(*p)) { ++p; continue; }
            if (*p == '#') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            break;
        }
        if (p >= end) return false;
        size_t n = 0;
        while (p < end && !std::isspace(*p) && *p != '#') {
            if (n + 1 < out_size) out[n++] = static_cast<char>(*p);
            ++p;
        }
        out[n] = '\0';
        return n > 0;
    }

    bool load(const uint8_t *begin, const uint8_t *end)
    {
        if (begin == nullptr || end == nullptr || end <= begin) return false;
        const uint8_t *p = begin;
        char token[24];
        if (!next_token(p, end, token, sizeof(token)) || std::strcmp(token, "P6") != 0) {
            ESP_LOGE(TAG, "Image must be binary PPM P6");
            return false;
        }
        if (!next_token(p, end, token, sizeof(token))) return false;
        const int w = std::atoi(token);
        if (!next_token(p, end, token, sizeof(token))) return false;
        const int h = std::atoi(token);
        if (!next_token(p, end, token, sizeof(token))) return false;
        const int max_value = std::atoi(token);
        if (w < DISPLAY_WIDTH || w > 2048 || h < IMAGE_VIEW_HEIGHT || h > 256 || max_value != 255) {
            ESP_LOGE(TAG, "Unsupported PPM dimensions/maxval: %dx%d max=%d (need width >= %d, height >= %d, max=255)",
                     w, h, max_value, DISPLAY_WIDTH, IMAGE_VIEW_HEIGHT);
            return false;
        }

        // P6 requires a whitespace separator after maxval. Consume exactly
        // that separator (and CRLF when present); do not skip pixel bytes.
        if (p >= end || !std::isspace(*p)) return false;
        if (*p == '\r' && (p + 1) < end && p[1] == '\n') p += 2;
        else ++p;

        const size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
        const size_t bytes_needed = pixel_count * 3U;
        if (static_cast<size_t>(end - p) < bytes_needed) {
            ESP_LOGE(TAG, "PPM pixel payload is truncated");
            return false;
        }

        width = w;
        height = h;
        rgb_data = p; // Embedded asset is read directly from memory-mapped flash.
        ESP_LOGI(TAG, "Embedded PPM loaded from flash: %dx%d", width, height);
        return true;
    }
};

void draw_scrolling_image(Hub75Driver &display, const PpmImage &image, int offset)
{
    if (!image.valid()) return;
    const int src_y_max = std::min(IMAGE_VIEW_HEIGHT, image.height);
    const int cur_offset = offset % image.width;
    // Keep the boundary column (x = 191) clear to prevent shift-register crosstalk into Panel 3
    constexpr int SCROLL_WIDTH = DISPLAY_WIDTH - 1;

    for (int y = 0; y < src_y_max; ++y) {
        const uint8_t *row_base =
            image.rgb_data + (static_cast<size_t>(y) * static_cast<size_t>(image.width)) * 3U;

        if (cur_offset + SCROLL_WIDTH <= image.width) {
            display.draw_pixels(0, static_cast<uint16_t>(y),
                                SCROLL_WIDTH, 1,
                                row_base + static_cast<size_t>(cur_offset) * 3U,
                                Hub75PixelFormat::RGB888, Hub75ColorOrder::RGB, false);
        } else {
            const int w1 = image.width - cur_offset;
            const int w2 = SCROLL_WIDTH - w1;

            display.draw_pixels(0, static_cast<uint16_t>(y),
                                static_cast<uint16_t>(w1), 1,
                                row_base + static_cast<size_t>(cur_offset) * 3U,
                                Hub75PixelFormat::RGB888, Hub75ColorOrder::RGB, false);

            display.draw_pixels(static_cast<uint16_t>(w1), static_cast<uint16_t>(y),
                                static_cast<uint16_t>(w2), 1,
                                row_base,
                                Hub75PixelFormat::RGB888, Hub75ColorOrder::RGB, false);
        }
    }
}

// ------------------------------ Ethernet ---------------------------------
constexpr gpio_num_t W5500_MISO = GPIO_NUM_12;
constexpr gpio_num_t W5500_MOSI = GPIO_NUM_11;
constexpr gpio_num_t W5500_SCLK = GPIO_NUM_13;
constexpr gpio_num_t W5500_CS   = GPIO_NUM_14;
constexpr gpio_num_t W5500_RST  = GPIO_NUM_9;
constexpr gpio_num_t W5500_INT  = GPIO_NUM_10;

constexpr EventBits_t BIT_ETH_LINK = BIT0;
constexpr EventBits_t BIT_IP_READY = BIT1;
EventGroupHandle_t g_network_events = nullptr;
esp_eth_handle_t g_eth_handle = nullptr;
esp_netif_t *g_eth_netif = nullptr;

void ethernet_event_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    if (event_id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "W5500 Ethernet link connected");
        xEventGroupSetBits(g_network_events, BIT_ETH_LINK);
#ifdef CONFIG_ETH_USE_STATIC_IP
        ESP_LOGI(TAG, "Static IP active: %s, Gateway: %s",
                 CONFIG_ETH_STATIC_IP_ADDR, CONFIG_ETH_STATIC_GATEWAY);
        xEventGroupSetBits(g_network_events, BIT_IP_READY);
#endif
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "W5500 Ethernet link disconnected");
        xEventGroupClearBits(g_network_events, BIT_ETH_LINK | BIT_IP_READY);
    } else if (event_id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "W5500 Ethernet driver started");
    } else if (event_id == ETHERNET_EVENT_STOP) {
        ESP_LOGW(TAG, "W5500 Ethernet driver stopped");
        xEventGroupClearBits(g_network_events, BIT_ETH_LINK | BIT_IP_READY);
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Ethernet IPv4: " IPSTR ", Mask: " IPSTR ", Gateway: " IPSTR,
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));

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

        if (event->ip_info.gw.addr != esp_ip4addr_aton("192.168.1.1")) {
            ESP_LOGW(TAG, "NOTICE: Gateway is " IPSTR " (not 192.168.1.1). If this is an unexpected subnet,",
                     IP2STR(&event->ip_info.gw));
            ESP_LOGW(TAG, "a rogue DHCP server on your switch may have answered first.");
            ESP_LOGW(TAG, "Enable 'Use Static IP' in menuconfig to force 192.168.1.1 gateway.");
        }

        xEventGroupSetBits(g_network_events, BIT_IP_READY);
    }
}

void init_w5500_ethernet()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Install GPIO ISR service to support W5500 interrupt
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_err));
        abort();
    }

    // Reset the onboard W5500. GPIO9 is the Waveshare ESP32-S3-ETH reset pin.
    gpio_config_t reset_cfg{};
    reset_cfg.pin_bit_mask = 1ULL << W5500_RST;
    reset_cfg.mode = GPIO_MODE_OUTPUT;
    reset_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    reset_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    reset_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&reset_cfg));
    ESP_ERROR_CHECK(gpio_set_level(W5500_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_level(W5500_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(200));

    spi_bus_config_t bus_cfg{};
    bus_cfg.mosi_io_num = W5500_MOSI;
    bus_cfg.miso_io_num = W5500_MISO;
    bus_cfg.sclk_io_num = W5500_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_device_cfg{};
    spi_device_cfg.mode = 0;
    spi_device_cfg.clock_speed_hz = 20 * 1000 * 1000;
    spi_device_cfg.spics_io_num = W5500_CS;
    spi_device_cfg.queue_size = 16;

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_device_cfg);
    w5500_cfg.int_gpio_num = W5500_INT;
    w5500_cfg.poll_period_ms = 0;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    if (mac == nullptr) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed. Check CONFIG_ETH_SPI_ETHERNET_W5500.");
        abort();
    }

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    // W5500 reset is handled with GPIO9 above. Do not let the generic PHY
    // config toggle the default GPIO5 (the board's TF-card SPI pin).
    phy_cfg.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (phy == nullptr) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        abort();
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &g_eth_handle));

    // W5500 does not have a factory MAC burned in EEPROM.
    // Read the Ethernet MAC address derived from ESP32 eFuse and set it on the driver.
    uint8_t base_mac_addr[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(base_mac_addr, ESP_MAC_ETH));
    ESP_LOGI(TAG, "Setting W5500 MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
             base_mac_addr[0], base_mac_addr[1], base_mac_addr[2],
             base_mac_addr[3], base_mac_addr[4], base_mac_addr[5]);
    ESP_ERROR_CHECK(esp_eth_ioctl(g_eth_handle, ETH_CMD_S_MAC_ADDR, base_mac_addr));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    g_eth_netif = esp_netif_new(&netif_cfg);
    if (g_eth_netif == nullptr) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        abort();
    }
    ESP_ERROR_CHECK(esp_netif_attach(g_eth_netif, esp_eth_new_netif_glue(g_eth_handle)));
    ESP_ERROR_CHECK(esp_netif_set_default_netif(g_eth_netif));

#ifdef CONFIG_ETH_USE_STATIC_IP
    ESP_LOGI(TAG, "Configuring Static IP: %s, Netmask: %s, Gateway: %s",
             CONFIG_ETH_STATIC_IP_ADDR, CONFIG_ETH_STATIC_NETMASK, CONFIG_ETH_STATIC_GATEWAY);
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(g_eth_netif));

    esp_netif_ip_info_t static_ip_info{};
    static_ip_info.ip.addr = esp_ip4addr_aton(CONFIG_ETH_STATIC_IP_ADDR);
    static_ip_info.netmask.addr = esp_ip4addr_aton(CONFIG_ETH_STATIC_NETMASK);
    static_ip_info.gw.addr = esp_ip4addr_aton(CONFIG_ETH_STATIC_GATEWAY);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(g_eth_netif, &static_ip_info));

    esp_netif_dns_info_t static_dns_main{};
    static_dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    static_dns_main.ip.u_addr.ip4.addr = esp_ip4addr_aton(CONFIG_ETH_STATIC_DNS_MAIN);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(g_eth_netif, ESP_NETIF_DNS_MAIN, &static_dns_main));

    esp_netif_dns_info_t static_dns_backup{};
    static_dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    static_dns_backup.ip.u_addr.ip4.addr = esp_ip4addr_aton(CONFIG_ETH_STATIC_DNS_BACKUP);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(g_eth_netif, ESP_NETIF_DNS_BACKUP, &static_dns_backup));
#endif

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                &ethernet_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                &ip_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(g_eth_handle));
#ifdef CONFIG_ETH_USE_STATIC_IP
    ESP_LOGI(TAG, "W5500 started with Static IP: %s; waiting for link", CONFIG_ETH_STATIC_IP_ADDR);
#else
    ESP_LOGI(TAG, "W5500 started; waiting for link and DHCP address");
#endif
}

// ----------------------- Weather / AQI network data -----------------------
struct DashboardData {
    bool has_weather = false;
    bool has_aqi = false;
    float temperature_c = 0.0f;
    int humidity_percent = 0;
    float wind_mps = 0.0f;
    int aqi_index = 0; // OpenWeatherMap category: 1..5 (not US EPA AQI)
    float pm25_ugm3 = 0.0f;
    char location[32] = "WAITING FOR NETWORK";
    char weather_description[48] = "WEATHER DATA PENDING";
    char weather_icon[8] = "";
    int weather_id = 0;
};

DashboardData g_dashboard;
SemaphoreHandle_t g_dashboard_mutex = nullptr;

struct HttpResponse {
    std::string body;
    bool overflow = false;
};

constexpr size_t MAX_HTTP_BODY = 8192;

esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        auto *response = static_cast<HttpResponse *>(event->user_data);
        if (response == nullptr) return ESP_FAIL;
        if (response->body.size() + static_cast<size_t>(event->data_len) > MAX_HTTP_BODY) {
            response->overflow = true;
            return ESP_FAIL;
        }
        response->body.append(static_cast<const char *>(event->data),
                              static_cast<size_t>(event->data_len));
    }
    return ESP_OK;
}

bool http_get_json(const std::string &url, std::string &body)
{
    HttpResponse response;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 512;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response.overflow) {
        ESP_LOGW(TAG, "HTTP request failed: err=%s status=%d overflow=%d body=%.120s",
                 esp_err_to_name(err), status, response.overflow ? 1 : 0,
                 response.body.c_str());
        return false;
    }
    body.swap(response.body);
    return true;
}

bool json_number(cJSON *object, const char *name, double &value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) return false;
    value = item->valuedouble;
    return true;
}

void copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0) return;
    std::snprintf(destination, destination_size, "%s", source ? source : "");
}

bool fetch_weather()
{
    // Use plain HTTP port 80 to prevent MBEDTLS_ERR_SSL_ALLOC_FAILED heap exhaustion,
    // as HUB75 DMA buffers already occupy ~295 KB of internal SRAM.
    const std::string url = std::string("http://api.openweathermap.org/data/2.5/weather?lat=") +
        CONFIG_WEATHER_LATITUDE + "&lon=" + CONFIG_WEATHER_LONGITUDE +
        "&appid=" + CONFIG_OPENWEATHER_API_KEY + "&units=metric";

    std::string body;
    if (!http_get_json(url, body)) return false;
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Weather JSON parse failed");
        return false;
    }

    cJSON *main = cJSON_GetObjectItemCaseSensitive(root, "main");
    cJSON *wind = cJSON_GetObjectItemCaseSensitive(root, "wind");
    cJSON *weather_array = cJSON_GetObjectItemCaseSensitive(root, "weather");
    cJSON *first_weather = cJSON_IsArray(weather_array) ? cJSON_GetArrayItem(weather_array, 0) : nullptr;

    double temperature = 0.0, humidity = 0.0, wind_speed = 0.0;
    const bool has_temp = json_number(main, "temp", temperature) &&
                          temperature > -80.0 && temperature < 80.0;
    const bool has_humidity = json_number(main, "humidity", humidity) &&
                              humidity >= 0.0 && humidity <= 100.0;
    const bool has_wind = json_number(wind, "speed", wind_speed) &&
                          wind_speed >= 0.0 && wind_speed < 200.0;
    cJSON *desc = cJSON_GetObjectItemCaseSensitive(first_weather, "description");
    cJSON *icon = cJSON_GetObjectItemCaseSensitive(first_weather, "icon");
    cJSON *wid = cJSON_GetObjectItemCaseSensitive(first_weather, "id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");

    if (has_temp && has_humidity) {
        xSemaphoreTake(g_dashboard_mutex, portMAX_DELAY);
        g_dashboard.temperature_c = static_cast<float>(temperature);
        g_dashboard.humidity_percent = static_cast<int>(humidity + 0.5);
        if (has_wind) g_dashboard.wind_mps = static_cast<float>(wind_speed);
        if (cJSON_IsString(desc)) copy_text(g_dashboard.weather_description,
                                             sizeof(g_dashboard.weather_description), desc->valuestring);
        if (cJSON_IsString(icon)) copy_text(g_dashboard.weather_icon,
                                             sizeof(g_dashboard.weather_icon), icon->valuestring);
        if (cJSON_IsNumber(wid)) g_dashboard.weather_id = wid->valueint;
        if (cJSON_IsString(name)) copy_text(g_dashboard.location,
                                             sizeof(g_dashboard.location), name->valuestring);
        g_dashboard.has_weather = true;
        xSemaphoreGive(g_dashboard_mutex);
        ESP_LOGI(TAG, "Weather updated: %.1f C, %d%% humidity",
                 temperature, static_cast<int>(humidity + 0.5));
    } else {
        ESP_LOGW(TAG, "Weather JSON did not contain main.temp/main.humidity");
    }

    cJSON_Delete(root);
    return has_temp && has_humidity;
}

const char *aqi_description(int aqi)
{
    switch (aqi) {
        case 1: return "GOOD";
        case 2: return "FAIR";
        case 3: return "MODERATE";
        case 4: return "POOR";
        case 5: return "VERY POOR";
        default: return "N/A";
    }
}

bool fetch_aqi()
{
    // Use plain HTTP port 80 to avoid SSL allocation failures
    const std::string url = std::string("http://api.openweathermap.org/data/2.5/air_pollution?lat=") +
        CONFIG_WEATHER_LATITUDE + "&lon=" + CONFIG_WEATHER_LONGITUDE +
        "&appid=" + CONFIG_OPENWEATHER_API_KEY;

    std::string body;
    if (!http_get_json(url, body)) return false;
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "AQI JSON parse failed");
        return false;
    }

    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    cJSON *first = cJSON_IsArray(list) ? cJSON_GetArrayItem(list, 0) : nullptr;
    cJSON *main = cJSON_GetObjectItemCaseSensitive(first, "main");
    cJSON *components = cJSON_GetObjectItemCaseSensitive(first, "components");
    double aqi = 0.0, pm25 = 0.0;
    const bool has_aqi = json_number(main, "aqi", aqi) &&
                         aqi >= 1.0 && aqi <= 5.0;
    const bool has_pm25 = json_number(components, "pm2_5", pm25) &&
                          pm25 >= 0.0 && pm25 < 10000.0;

    if (has_aqi) {
        xSemaphoreTake(g_dashboard_mutex, portMAX_DELAY);
        g_dashboard.aqi_index = static_cast<int>(aqi);
        if (has_pm25) g_dashboard.pm25_ugm3 = static_cast<float>(pm25);
        g_dashboard.has_aqi = true;
        xSemaphoreGive(g_dashboard_mutex);
        ESP_LOGI(TAG, "AQI updated: %d (%s), PM2.5 %.1f ug/m3",
                 static_cast<int>(aqi), aqi_description(static_cast<int>(aqi)), pm25);
    } else {
        ESP_LOGW(TAG, "AQI JSON did not contain list[0].main.aqi");
    }

    cJSON_Delete(root);
    return has_aqi;
}

void start_sntp_once()
{
    static bool started = false;
    if (started) return;

    setenv("TZ", "IST-5:30", 1); // India Standard Time (UTC+05:30)
    tzset();

    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        started = true;
        const esp_err_t sync_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(12000));
        if (sync_err == ESP_OK) ESP_LOGI(TAG, "SNTP time synchronized");
        else ESP_LOGW(TAG, "SNTP sync not completed yet: %s", esp_err_to_name(sync_err));
    } else if (err == ESP_ERR_INVALID_STATE) {
        started = true;
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
}

void weather_task(void *arg)
{
    (void)arg;
    bool have_api_key_warning = false;

    while (true) {
        xEventGroupWaitBits(g_network_events, BIT_IP_READY, pdFALSE, pdTRUE, portMAX_DELAY);
        start_sntp_once();

        bool refresh_ok = false;
        if (std::strlen(CONFIG_OPENWEATHER_API_KEY) == 0) {
            if (!have_api_key_warning) {
                ESP_LOGW(TAG, "OpenWeatherMap API key is empty. Set it in idf.py menuconfig.");
                have_api_key_warning = true;
            }
        } else {
            const bool weather_ok = fetch_weather();
            const bool aqi_ok = fetch_aqi();
            refresh_ok = weather_ok && aqi_ok;
        }

        const uint32_t delay_seconds = refresh_ok
            ? static_cast<uint32_t>(CONFIG_WEATHER_UPDATE_INTERVAL_MIN) * 60U
            : 60U; // Retry network/API/NTP failures after one minute.

        // Sleep in 1-second slices so link loss is noticed promptly.
        for (uint32_t elapsed = 0; elapsed < delay_seconds; ++elapsed) {
            if ((xEventGroupGetBits(g_network_events) & BIT_IP_READY) == 0) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// ------------------------------ Rendering --------------------------------
void get_aqi_color(int aqi, uint8_t &r, uint8_t &g, uint8_t &b)
{
    // OpenWeatherMap AQI category palette: 1 good -> 5 very poor.
    switch (aqi) {
        case 1: r = 0;   g = 255; b = 40;  break;
        case 2: r = 180; g = 255; b = 0;   break;
        case 3: r = 255; g = 150; b = 0;   break;
        case 4: r = 255; g = 50;  b = 0;   break;
        case 5: r = 210; g = 0;   b = 180; break;
        default:r = 180; g = 180; b = 180;break;
    }
}

// ---------------------------- Pixel Art Icons -----------------------------
struct RgbColor {
    uint8_t r, g, b;
};

struct Icon8x8 {
    RgbColor palette[4];
    uint8_t p[64];
};

struct Icon5x7 {
    RgbColor palette[4];
    uint8_t p[35];
};

static constexpr Icon8x8 ICON_SUN = {
    {{0, 0, 0}, {255, 140, 0}, {255, 215, 0}, {255, 255, 180}},
    {
        0, 0, 1, 0, 0, 1, 0, 0,
        0, 1, 2, 2, 2, 2, 1, 0,
        1, 2, 3, 3, 3, 3, 2, 1,
        0, 2, 3, 3, 3, 3, 2, 0,
        0, 2, 3, 3, 3, 3, 2, 0,
        1, 2, 3, 3, 3, 3, 2, 1,
        0, 1, 2, 2, 2, 2, 1, 0,
        0, 0, 1, 0, 0, 1, 0, 0,
    }
};

static constexpr Icon8x8 ICON_MOON = {
    {{0, 0, 0}, {240, 200, 30}, {255, 240, 150}, {180, 220, 255}},
    {
        0, 0, 0, 1, 1, 0, 0, 0,
        0, 0, 1, 2, 2, 1, 0, 3,
        0, 1, 2, 2, 1, 0, 0, 0,
        0, 1, 2, 2, 0, 0, 0, 0,
        0, 1, 2, 2, 0, 0, 0, 0,
        0, 1, 2, 2, 1, 0, 0, 0,
        0, 0, 1, 2, 2, 1, 0, 0,
        0, 0, 0, 1, 1, 0, 0, 0,
    }
};

static constexpr Icon8x8 ICON_SUN_CLOUD = {
    {{0, 0, 0}, {255, 180, 0}, {230, 235, 245}, {140, 160, 185}},
    {
        0, 1, 1, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 0, 0, 0, 0,
        0, 1, 1, 2, 2, 2, 0, 0,
        0, 0, 2, 2, 2, 2, 2, 0,
        0, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2,
        0, 3, 3, 3, 3, 3, 3, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    }
};

static constexpr Icon8x8 ICON_CLOUD = {
    {{0, 0, 0}, {240, 245, 255}, {170, 185, 205}, {110, 125, 145}},
    {
        0, 0, 0, 1, 1, 0, 0, 0,
        0, 1, 1, 1, 1, 1, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 0,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        0, 2, 2, 2, 2, 2, 2, 0,
        0, 0, 3, 3, 3, 3, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    }
};

static constexpr Icon8x8 ICON_RAIN = {
    {{0, 0, 0}, {180, 195, 210}, {120, 135, 155}, {0, 180, 255}},
    {
        0, 0, 1, 1, 1, 0, 0, 0,
        0, 1, 1, 1, 1, 1, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 0,
        0, 2, 2, 2, 2, 2, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 3, 0, 3, 0, 3, 0, 0,
        0, 3, 0, 3, 0, 3, 0, 0,
        3, 0, 3, 0, 3, 0, 0, 0,
    }
};

static constexpr Icon8x8 ICON_THUNDER = {
    {{0, 0, 0}, {130, 140, 160}, {255, 230, 0}, {255, 140, 0}},
    {
        0, 1, 1, 1, 1, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 0, 0,
        0, 1, 1, 1, 1, 0, 0, 0,
        0, 0, 0, 2, 2, 0, 0, 0,
        0, 0, 2, 2, 0, 0, 0, 0,
        0, 0, 0, 2, 2, 2, 0, 0,
        0, 0, 0, 0, 2, 0, 0, 0,
        0, 0, 0, 3, 0, 0, 0, 0,
    }
};

static constexpr Icon8x8 ICON_SNOW = {
    {{0, 0, 0}, {170, 185, 210}, {160, 230, 255}, {255, 255, 255}},
    {
        0, 1, 1, 1, 1, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 0, 0,
        0, 1, 1, 1, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 3, 0, 2, 0, 3, 0, 0,
        2, 0, 3, 3, 3, 0, 2, 0,
        0, 3, 0, 2, 0, 3, 0, 0,
        0, 0, 2, 0, 2, 0, 0, 0,
    }
};

static constexpr Icon8x8 ICON_MIST = {
    {{0, 0, 0}, {200, 215, 230}, {140, 160, 180}, {90, 110, 130}},
    {
        0, 1, 1, 1, 1, 1, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 1,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 2, 2, 2, 2, 2, 2, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 3, 3, 3, 3, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    }
};

static constexpr Icon5x7 ICON_THERMO = {
    {{0, 0, 0}, {255, 60, 60}, {220, 220, 220}, {255, 180, 180}},
    {
        0, 2, 2, 2, 0,
        0, 2, 1, 2, 0,
        0, 2, 1, 2, 0,
        0, 2, 1, 2, 0,
        2, 1, 1, 1, 2,
        2, 1, 3, 1, 2,
        0, 2, 2, 2, 0,
    }
};

static constexpr Icon5x7 ICON_DROPLET = {
    {{0, 0, 0}, {0, 180, 255}, {160, 235, 255}, {0, 100, 210}},
    {
        0, 0, 1, 0, 0,
        0, 1, 2, 1, 0,
        0, 1, 2, 1, 0,
        1, 1, 2, 1, 1,
        1, 2, 2, 1, 1,
        1, 1, 1, 3, 3,
        0, 3, 3, 3, 0,
    }
};

void draw_icon_8x8(Hub75Driver &display, const Icon8x8 &icon, int x, int y)
{
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            const uint8_t idx = icon.p[r * 8 + c];
            if (idx == 0) continue;
            const auto &col = icon.palette[idx];
            const int px = x + c;
            const int py = y + r;
            if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                display.set_pixel(static_cast<uint16_t>(px), static_cast<uint16_t>(py),
                                  col.r, col.g, col.b);
            }
        }
    }
}

void draw_icon_5x7(Hub75Driver &display, const Icon5x7 &icon, int x, int y)
{
    for (int r = 0; r < 7; ++r) {
        for (int c = 0; c < 5; ++c) {
            const uint8_t idx = icon.p[r * 5 + c];
            if (idx == 0) continue;
            const auto &col = icon.palette[idx];
            const int px = x + c;
            const int py = y + r;
            if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                display.set_pixel(static_cast<uint16_t>(px), static_cast<uint16_t>(py),
                                  col.r, col.g, col.b);
            }
        }
    }
}

const Icon8x8 &get_weather_icon(const char *icon_code, int weather_id)
{
    if (icon_code && icon_code[0] != '\0') {
        if (std::strcmp(icon_code, "01d") == 0) return ICON_SUN;
        if (std::strcmp(icon_code, "01n") == 0) return ICON_MOON;
        if (std::strncmp(icon_code, "02", 2) == 0) return ICON_SUN_CLOUD;
        if (std::strncmp(icon_code, "03", 2) == 0 || std::strncmp(icon_code, "04", 2) == 0) return ICON_CLOUD;
        if (std::strncmp(icon_code, "09", 2) == 0 || std::strncmp(icon_code, "10", 2) == 0) return ICON_RAIN;
        if (std::strncmp(icon_code, "11", 2) == 0) return ICON_THUNDER;
        if (std::strncmp(icon_code, "13", 2) == 0) return ICON_SNOW;
        if (std::strncmp(icon_code, "50", 2) == 0) return ICON_MIST;
    }
    // Fallback to weather_id
    if (weather_id >= 200 && weather_id < 300) return ICON_THUNDER;
    if (weather_id >= 300 && weather_id < 600) return ICON_RAIN;
    if (weather_id >= 600 && weather_id < 700) return ICON_SNOW;
    if (weather_id >= 700 && weather_id < 800) return ICON_MIST;
    if (weather_id == 800) return ICON_SUN;
    if (weather_id > 800) return ICON_CLOUD;
    return ICON_SUN;
}

constexpr int TEXT_MARGIN_X = 4; // Spacing 4 LEDs from left bezel to prevent right-edge clipping

void draw_dashboard(Hub75Driver &display, const PpmImage &image,
                    int image_offset, const DashboardData &data)
{
    // Fully clear active buffer to black across all physical rows and DMA width.
    // This wipes stale double-buffer data and eliminates random flickering in the black area.
    display.clear();

    // Draw scrolling image in the top 192x48 area
    draw_scrolling_image(display, image, image_offset);

    // Separator line between scrolling image and data has been removed per request.

    // Line 1: Date, Time & City Name
    char line1[64] = "TIME SYNCING";
    char time_text[24] = {};
    time_t now = time(nullptr);
    struct tm local_tm{};
    localtime_r(&now, &local_tm);
    if (local_tm.tm_year >= (2024 - 1900)) {
        std::strftime(time_text, sizeof(time_text), "%a %d %b %H:%M", &local_tm);
        std::snprintf(line1, sizeof(line1), "%s %s", time_text,
                      data.has_weather ? data.location : "");
    }
    draw_text(display, line1, TEXT_MARGIN_X, 50, 230, 230, 230);

    // Line 2: Weather icon + Condition description + Wind speed
    if (data.has_weather) {
        draw_icon_8x8(display, get_weather_icon(data.weather_icon, data.weather_id),
                      TEXT_MARGIN_X, 59);
        char line2[64];
        std::snprintf(line2, sizeof(line2), "%s  W:%.1fM/S",
                      data.weather_description, data.wind_mps);
        draw_text(display, line2, TEXT_MARGIN_X + 10, 60, 255, 210, 40);
    } else {
        draw_text(display, "WX WAITING FOR DATA", TEXT_MARGIN_X, 60, 255, 210, 40);
    }

    // Line 3: Thermometer icon + Temperature, Droplet icon + Humidity
    if (data.has_weather) {
        draw_icon_5x7(display, ICON_THERMO, TEXT_MARGIN_X, 70);
        char temp_str[16];
        std::snprintf(temp_str, sizeof(temp_str), "%.1fC", data.temperature_c);
        draw_text(display, temp_str, TEXT_MARGIN_X + 7, 70, 255, 120, 120);

        draw_icon_5x7(display, ICON_DROPLET, TEXT_MARGIN_X + 55, 70);
        char hum_str[16];
        std::snprintf(hum_str, sizeof(hum_str), "%d%%", data.humidity_percent);
        draw_text(display, hum_str, TEXT_MARGIN_X + 62, 70, 0, 220, 255);
    } else {
        draw_text(display, "TEMP:N/A HUM:N/A", TEXT_MARGIN_X, 70, 0, 220, 255);
    }

    // Line 4: Air Quality Index + PM2.5
    char line4[64];
    if (data.has_aqi) {
        std::snprintf(line4, sizeof(line4), "AQI:%d %s PM2.5:%.1f",
                      data.aqi_index, aqi_description(data.aqi_index), data.pm25_ugm3);
    } else {
        std::snprintf(line4, sizeof(line4), "AQI:N/A PM2.5:N/A");
    }
    uint8_t aqi_r, aqi_g, aqi_b;
    get_aqi_color(data.has_aqi ? data.aqi_index : 0, aqi_r, aqi_g, aqi_b);
    draw_text(display, line4, TEXT_MARGIN_X, 80, aqi_r, aqi_g, aqi_b);
}

void display_task(void *arg)
{
    auto *ctx = static_cast<std::pair<Hub75Driver *, PpmImage *> *>(arg);
    Hub75Driver &display = *ctx->first;
    const PpmImage &image = *ctx->second;

    int image_offset = 0;
    while (true) {
        DashboardData snapshot;
        if (xSemaphoreTake(g_dashboard_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            snapshot = g_dashboard;
            xSemaphoreGive(g_dashboard_mutex);
        }

        draw_dashboard(display, image, image_offset, snapshot);
        display.flip_buffer();

        image_offset += IMAGE_SCROLL_STEP;
        if (image.valid() && image_offset >= image.width) image_offset = 0;
        vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
    }
}

} // namespace

extern "C" void app_main(void)
{
    g_network_events = xEventGroupCreate();
    g_dashboard_mutex = xSemaphoreCreateMutex();
    if (g_network_events == nullptr || g_dashboard_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS synchronization objects");
        abort();
    }

    Hub75Config display_config = make_display_config();
    static Hub75Driver display(display_config);
    ESP_LOGI(TAG, "Starting 192x96 HUB75 weather/image dashboard");
    if (!display.begin()) {
        ESP_LOGE(TAG, "HUB75 driver initialization failed");
        abort();
    }
    display.set_brightness(DISPLAY_BRIGHTNESS);

    // Soften slew rate on clock and latch to suppress cable transmission-line ringing and crosstalk
    gpio_set_drive_capability(GPIO_NUM_21, GPIO_DRIVE_CAP_2); // CLK
    gpio_set_drive_capability(GPIO_NUM_47, GPIO_DRIVE_CAP_2); // LAT

    static PpmImage image;
    if (!image.load(image_ppm_start, image_ppm_end)) {
        ESP_LOGE(TAG, "Failed to load embedded image.ppm; replace it with a valid P6 PPM");
        abort();
    }

    // Start drawing immediately. The image and N/A placeholders remain visible
    // while Ethernet negotiates and the weather task performs HTTPS requests.
    static std::pair<Hub75Driver *, PpmImage *> display_context{&display, &image};
    if (xTaskCreatePinnedToCore(display_task, "matrix_display", 6144,
                                &display_context, 3, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create display task");
        abort();
    }

    init_w5500_ethernet();
    if (xTaskCreatePinnedToCore(weather_task, "weather_fetch", 10240,
                                nullptr, 3, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create weather task");
        abort();
    }
}
