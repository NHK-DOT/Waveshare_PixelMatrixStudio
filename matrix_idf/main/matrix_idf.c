#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "soc/gpio_struct.h"

#include "wifi_config.h"

#define HUB75_WIDTH 64
#define HUB75_HEIGHT 64
#define HUB75_SCAN_ROWS 32
#define HUB75_FRAME_BYTES (HUB75_WIDTH * HUB75_HEIGHT)
#define HUB75_PWM_PLANES 3
#define HUB75_BASE_DWELL_US 20

/* HUB75 input connector mapping for this classic ESP32 board. */
#define HUB75_R1_GPIO 25
#define HUB75_G1_GPIO 26
#define HUB75_B1_GPIO 27
#define HUB75_R2_GPIO 14
#define HUB75_G2_GPIO 18
#define HUB75_B2_GPIO 13
#define HUB75_A_GPIO 23
#define HUB75_B_GPIO 22
#define HUB75_C_GPIO 19
#define HUB75_D_GPIO 17
#define HUB75_E_GPIO 32
#define HUB75_LAT_GPIO 21
#define HUB75_OE_GPIO 33
#define HUB75_CLK_GPIO 16

#define GPIO_BIT(gpio) (1U << (gpio))
#define HUB75_RGB_MASK (GPIO_BIT(HUB75_R1_GPIO) | GPIO_BIT(HUB75_G1_GPIO) | \
                        GPIO_BIT(HUB75_B1_GPIO) | GPIO_BIT(HUB75_R2_GPIO) | \
                        GPIO_BIT(HUB75_G2_GPIO) | GPIO_BIT(HUB75_B2_GPIO))
#define HUB75_ADDR_LOW_MASK (GPIO_BIT(HUB75_A_GPIO) | GPIO_BIT(HUB75_B_GPIO) | \
                             GPIO_BIT(HUB75_C_GPIO) | GPIO_BIT(HUB75_D_GPIO))
#define HUB75_E_HIGH_MASK (1U << (HUB75_E_GPIO - 32))
#define HUB75_OE_HIGH_MASK (1U << (HUB75_OE_GPIO - 32))

#define PMX_VERSION 1
#define PMX_FORMAT_RGB332 1
#define PMX_ASSET_SUBTYPE 0x40
#define PMX_MAX_CACHE_BYTES (240U * 1024U)
#define PMX_MAX_CACHE_FRAMES (PMX_MAX_CACHE_BYTES / HUB75_FRAME_BYTES)

static const char *TAG = "PMX_PLAYER";

extern const uint8_t web_index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_web_index_html_end");

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t format;
    uint8_t width;
    uint8_t height;
    uint32_t frame_count;
    uint32_t directory_offset;
    uint32_t data_offset;
    uint32_t file_size;
} pmx_header_t;

typedef struct __attribute__((packed)) {
    uint32_t delay_ms;
    uint32_t data_offset;
} pmx_frame_entry_t;

static uint8_t s_framebuffers[2][HUB75_FRAME_BYTES];
static uint8_t s_decode_buffer[HUB75_FRAME_BYTES];
static uint8_t *s_cached_frames;
static uint32_t s_cached_delays[PMX_MAX_CACHE_FRAMES];
static uint32_t s_cached_frame_count;
static uint8_t *s_pending_frames;
static uint32_t s_pending_delays[PMX_MAX_CACHE_FRAMES];
static uint32_t s_pending_frame_count;
static volatile bool s_pending_cache_ready;
static volatile uint8_t s_front_buffer;
static volatile bool s_assets_updating;
static volatile bool s_http_uploading;
static volatile bool s_display_paused;
static volatile uint32_t s_pmx_generation;
static httpd_handle_t s_http_server;
static SemaphoreHandle_t s_cache_mutex;

/* The installed panel maps raw scan coordinates counterclockwise by 90 degrees. */
static void panel_copy_display_frame(uint8_t *scan_frame, const uint8_t *display_frame)
{
    for (int y = 0; y < HUB75_HEIGHT; ++y) {
        for (int x = 0; x < HUB75_WIDTH; ++x) {
            scan_frame[x * HUB75_WIDTH + (HUB75_WIDTH - 1 - y)] =
                display_frame[y * HUB75_WIDTH + x];
        }
    }
}

static void panel_set_display_pixel(uint8_t *scan_frame, int x, int y, uint8_t color)
{
    scan_frame[x * HUB75_WIDTH + (HUB75_WIDTH - 1 - y)] = color;
}

static void IRAM_ATTR hub75_set_address(uint8_t row)
{
    uint32_t low_value = 0;
    if (row & 0x01) low_value |= GPIO_BIT(HUB75_A_GPIO);
    if (row & 0x02) low_value |= GPIO_BIT(HUB75_B_GPIO);
    if (row & 0x04) low_value |= GPIO_BIT(HUB75_C_GPIO);
    if (row & 0x08) low_value |= GPIO_BIT(HUB75_D_GPIO);

    GPIO.out_w1tc = HUB75_ADDR_LOW_MASK;
    GPIO.out_w1ts = low_value;
    GPIO.out1_w1tc.val = HUB75_E_HIGH_MASK;
    if (row & 0x10) GPIO.out1_w1ts.val = HUB75_E_HIGH_MASK;
}

static void IRAM_ATTR hub75_set_rgb_bits(uint8_t top, uint8_t bottom, uint8_t plane)
{
    static const DRAM_ATTR uint8_t blue_rgb332_to_3bit[4] = {0, 2, 5, 7};
    const uint8_t top_blue = blue_rgb332_to_3bit[top & 0x03];
    const uint8_t bottom_blue = blue_rgb332_to_3bit[bottom & 0x03];
    uint32_t rgb_value = 0;

    if ((top >> (5 + plane)) & 1U) rgb_value |= GPIO_BIT(HUB75_R1_GPIO);
    if ((bottom >> (5 + plane)) & 1U) rgb_value |= GPIO_BIT(HUB75_R2_GPIO);
    if ((top >> (2 + plane)) & 1U) rgb_value |= GPIO_BIT(HUB75_G1_GPIO);
    if ((bottom >> (2 + plane)) & 1U) rgb_value |= GPIO_BIT(HUB75_G2_GPIO);
    if ((top_blue >> plane) & 1U) rgb_value |= GPIO_BIT(HUB75_B1_GPIO);
    if ((bottom_blue >> plane) & 1U) rgb_value |= GPIO_BIT(HUB75_B2_GPIO);

    GPIO.out_w1tc = HUB75_RGB_MASK;
    GPIO.out_w1ts = rgb_value;
}

static void IRAM_ATTR hub75_clock_pulse(void)
{
    GPIO.out_w1ts = GPIO_BIT(HUB75_CLK_GPIO);
    GPIO.out_w1tc = GPIO_BIT(HUB75_CLK_GPIO);
}

static void hub75_fm6124_init(void)
{
    static const bool register_1[16] = {
        false, false, false, false, false, true, true, true,
        true, true, true, false, false, false, false, false,
    };
    static const bool register_2[16] = {
        false, false, false, false, false, false, false, false,
        false, true, false, false, false, false, false, false,
    };

    GPIO.out1_w1ts.val = HUB75_OE_HIGH_MASK;
    for (int column = 0; column < HUB75_WIDTH; ++column) {
        if (register_1[column % 16]) GPIO.out_w1ts = HUB75_RGB_MASK;
        else GPIO.out_w1tc = HUB75_RGB_MASK;
        if (column > HUB75_WIDTH - 12) GPIO.out_w1ts = GPIO_BIT(HUB75_LAT_GPIO);
        hub75_clock_pulse();
    }
    GPIO.out_w1tc = GPIO_BIT(HUB75_LAT_GPIO);

    for (int column = 0; column < HUB75_WIDTH; ++column) {
        if (register_2[column % 16]) GPIO.out_w1ts = HUB75_RGB_MASK;
        else GPIO.out_w1tc = HUB75_RGB_MASK;
        if (column > HUB75_WIDTH - 13) GPIO.out_w1ts = GPIO_BIT(HUB75_LAT_GPIO);
        hub75_clock_pulse();
    }
    GPIO.out_w1tc = GPIO_BIT(HUB75_LAT_GPIO);
    GPIO.out_w1tc = HUB75_RGB_MASK;
    for (int column = 0; column < HUB75_WIDTH; ++column) hub75_clock_pulse();
    GPIO.out_w1ts = GPIO_BIT(HUB75_LAT_GPIO);
    hub75_clock_pulse();
    GPIO.out_w1tc = GPIO_BIT(HUB75_LAT_GPIO);
    GPIO.out1_w1tc.val = HUB75_OE_HIGH_MASK;
}

static void IRAM_ATTR hub75_scan_row(uint8_t row, uint8_t plane)
{
    const uint8_t *frame = s_framebuffers[s_front_buffer];
    GPIO.out1_w1ts.val = HUB75_OE_HIGH_MASK;
    hub75_set_address(row);

    for (int column = 0; column < HUB75_WIDTH; ++column) {
        const uint8_t top = frame[row * HUB75_WIDTH + column];
        const uint8_t bottom = frame[(row + HUB75_SCAN_ROWS) * HUB75_WIDTH + column];
        hub75_set_rgb_bits(top, bottom, plane);
        hub75_clock_pulse();
    }

    GPIO.out_w1ts = GPIO_BIT(HUB75_LAT_GPIO);
    GPIO.out_w1tc = GPIO_BIT(HUB75_LAT_GPIO);
    GPIO.out1_w1tc.val = HUB75_OE_HIGH_MASK;
    esp_rom_delay_us(HUB75_BASE_DWELL_US << plane);
    GPIO.out1_w1ts.val = HUB75_OE_HIGH_MASK;
}

static void hub75_refresh_task(void *argument)
{
    (void)argument;
    while (true) {
        if (s_display_paused) {
            GPIO.out1_w1ts.val = HUB75_OE_HIGH_MASK;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        for (uint8_t plane = 0; plane < HUB75_PWM_PLANES; ++plane) {
            for (uint8_t row = 0; row < HUB75_SCAN_ROWS; ++row) hub75_scan_row(row, plane);
        }
    }
}

static void hub75_init(void)
{
    const uint64_t pin_mask =
        (1ULL << HUB75_R1_GPIO) | (1ULL << HUB75_G1_GPIO) | (1ULL << HUB75_B1_GPIO) |
        (1ULL << HUB75_R2_GPIO) | (1ULL << HUB75_G2_GPIO) | (1ULL << HUB75_B2_GPIO) |
        (1ULL << HUB75_A_GPIO) | (1ULL << HUB75_B_GPIO) | (1ULL << HUB75_C_GPIO) |
        (1ULL << HUB75_D_GPIO) | (1ULL << HUB75_E_GPIO) | (1ULL << HUB75_LAT_GPIO) |
        (1ULL << HUB75_OE_GPIO) | (1ULL << HUB75_CLK_GPIO);
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));
    GPIO.out_w1tc = HUB75_RGB_MASK | HUB75_ADDR_LOW_MASK | GPIO_BIT(HUB75_CLK_GPIO) |
                    GPIO_BIT(HUB75_LAT_GPIO);
    GPIO.out1_w1ts.val = HUB75_OE_HIGH_MASK;
    GPIO.out1_w1tc.val = HUB75_E_HIGH_MASK;
    hub75_fm6124_init();
}

static void show_builtin_test_pattern(void)
{
    memset(s_framebuffers[0], 0, HUB75_FRAME_BYTES);
    for (int y = 0; y < HUB75_HEIGHT; ++y) {
        for (int x = 0; x < HUB75_WIDTH; ++x) {
            uint8_t color = x < 22 ? 0xE0 : (x < 43 ? 0x1C : 0x03);
            panel_set_display_pixel(s_framebuffers[0], x, y, color);
        }
    }
    memcpy(s_framebuffers[1], s_framebuffers[0], HUB75_FRAME_BYTES);
}

static bool pmx_header_is_valid(const pmx_header_t *header, size_t capacity, size_t expected_size)
{
    if (memcmp(header->magic, "PMX1", 4) != 0 || header->version != PMX_VERSION ||
        header->format != PMX_FORMAT_RGB332 || header->width != HUB75_WIDTH ||
        header->height != HUB75_HEIGHT || header->frame_count == 0) return false;
    if (header->file_size != expected_size || header->file_size > capacity ||
        header->directory_offset < sizeof(*header) || header->data_offset < header->directory_offset ||
        header->data_offset > header->file_size) return false;
    if (header->frame_count > (header->data_offset - header->directory_offset) / sizeof(pmx_frame_entry_t)) return false;
    return true;
}

static bool pmx_read_header(const esp_partition_t *assets, pmx_header_t *header)
{
    if (assets == NULL || assets->size < sizeof(*header)) return false;
    if (esp_partition_read(assets, 0, header, sizeof(*header)) != ESP_OK) return false;
    return pmx_header_is_valid(header, assets->size, header->file_size);
}

static bool pmx_load_frame(const esp_partition_t *assets, const pmx_header_t *header, uint32_t frame_index,
                           uint32_t *delay_ms)
{
    pmx_frame_entry_t entry;
    if (esp_partition_read(assets, header->directory_offset + frame_index * sizeof(entry),
                           &entry, sizeof(entry)) != ESP_OK) return false;
    if (entry.data_offset < header->data_offset || entry.data_offset > header->file_size - HUB75_FRAME_BYTES) return false;

    const uint8_t back_buffer = s_front_buffer ^ 1U;
    if (esp_partition_read(assets, entry.data_offset, s_decode_buffer, HUB75_FRAME_BYTES) != ESP_OK) return false;
    panel_copy_display_frame(s_framebuffers[back_buffer], s_decode_buffer);
    s_front_buffer = back_buffer;
    *delay_ms = entry.delay_ms < 20 ? 20 : entry.delay_ms;
    return true;
}

static bool pmx_read_frame_to_scan_buffer(const esp_partition_t *assets, const pmx_header_t *header,
                                          uint32_t frame_index, uint8_t *scan_frame, uint32_t *delay_ms)
{
    pmx_frame_entry_t entry;
    if (esp_partition_read(assets, header->directory_offset + frame_index * sizeof(entry),
                           &entry, sizeof(entry)) != ESP_OK) return false;
    if (entry.data_offset < header->data_offset || entry.data_offset > header->file_size - HUB75_FRAME_BYTES) return false;
    if (esp_partition_read(assets, entry.data_offset, s_decode_buffer, HUB75_FRAME_BYTES) != ESP_OK) return false;

    panel_copy_display_frame(scan_frame, s_decode_buffer);
    *delay_ms = entry.delay_ms < 20 ? 20 : entry.delay_ms;
    return true;
}

static bool pmx_cache_frames(const esp_partition_t *assets, const pmx_header_t *header)
{
    if (header->frame_count > PMX_MAX_CACHE_FRAMES) {
        ESP_LOGW(TAG, "PMX has %lu frames; RAM cache limit is %lu frames. Falling back to flash streaming.",
                 (unsigned long)header->frame_count, (unsigned long)PMX_MAX_CACHE_FRAMES);
        return false;
    }

    const size_t cache_bytes = (size_t)header->frame_count * HUB75_FRAME_BYTES;
    uint8_t *frames = (uint8_t *)malloc(cache_bytes);
    if (frames == NULL) {
        ESP_LOGW(TAG, "Unable to allocate %lu bytes for PMX RAM cache. Falling back to flash streaming.",
                 (unsigned long)cache_bytes);
        return false;
    }

    for (uint32_t frame_index = 0; frame_index < header->frame_count; ++frame_index) {
        if (!pmx_read_frame_to_scan_buffer(assets, header, frame_index,
                                           frames + frame_index * HUB75_FRAME_BYTES,
                                           &s_cached_delays[frame_index])) {
            free(frames);
            ESP_LOGE(TAG, "Unable to cache PMX frame %lu", (unsigned long)frame_index);
            return false;
        }
    }

    if (s_cache_mutex != NULL) xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    uint8_t *old_frames = s_cached_frames;
    s_cached_frames = frames;
    s_cached_frame_count = header->frame_count;
    if (s_cache_mutex != NULL) xSemaphoreGive(s_cache_mutex);
    free(old_frames);
    ESP_LOGI(TAG, "PMX cached in RAM: %lu frames, %lu bytes",
             (unsigned long)s_cached_frame_count, (unsigned long)cache_bytes);
    return true;
}

static bool pmx_adopt_pending_cache(void)
{
    if (!s_pending_cache_ready) return false;
    if (s_pending_frames == NULL || s_pending_frame_count == 0 ||
        s_pending_frame_count > PMX_MAX_CACHE_FRAMES) {
        s_pending_cache_ready = false;
        return false;
    }

    if (s_cache_mutex != NULL) xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    uint8_t *old_frames = s_cached_frames;
    s_cached_frames = s_pending_frames;
    s_cached_frame_count = s_pending_frame_count;
    memcpy(s_cached_delays, s_pending_delays,
           s_cached_frame_count * sizeof(s_cached_delays[0]));

    s_pending_frames = NULL;
    s_pending_frame_count = 0;
    s_pending_cache_ready = false;
    if (s_cache_mutex != NULL) xSemaphoreGive(s_cache_mutex);
    free(old_frames);

    ESP_LOGI(TAG, "HTTP PMX RAM cache adopted: %lu frames",
             (unsigned long)s_cached_frame_count);
    return true;
}

static void pmx_release_current_cache(void)
{
    if (s_cache_mutex != NULL) xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    uint8_t *old_frames = s_cached_frames;
    s_cached_frames = NULL;
    s_cached_frame_count = 0;
    if (s_cache_mutex != NULL) xSemaphoreGive(s_cache_mutex);
    free(old_frames);
}

static void pmx_show_cached_frame(uint32_t frame_index)
{
    if (s_cache_mutex != NULL) xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (s_cached_frames == NULL || frame_index >= s_cached_frame_count) {
        if (s_cache_mutex != NULL) xSemaphoreGive(s_cache_mutex);
        return;
    }
    const uint8_t *source = s_cached_frames + frame_index * HUB75_FRAME_BYTES;
    const uint8_t back_buffer = s_front_buffer ^ 1U;
    memcpy(s_framebuffers[back_buffer], source, HUB75_FRAME_BYTES);
    s_front_buffer = back_buffer;
    if (s_cache_mutex != NULL) xSemaphoreGive(s_cache_mutex);
}

static bool http_receive_exact(httpd_req_t *request, char *buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        const int count = httpd_req_recv(request, buffer + received, length - received);
        if (count <= 0) return false;
        received += (size_t)count;
    }
    return true;
}

static esp_err_t http_root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, (const char *)web_index_html_start,
                           web_index_html_end - web_index_html_start);
}

static esp_err_t http_ping_handler(httpd_req_t *request)
{
    char response[160];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"uploading\":%s,\"pending\":%s,\"pmx_generation\":%lu,\"cached_frames\":%lu}",
             s_http_uploading ? "true" : "false",
             s_pending_cache_ready ? "true" : "false",
             (unsigned long)s_pmx_generation,
             (unsigned long)s_cached_frame_count);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t http_upload_handler(httpd_req_t *request)
{
    if (s_pending_cache_ready || s_http_uploading) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Previous upload is still being adopted");
        return ESP_FAIL;
    }

    const esp_partition_t *assets = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        (esp_partition_subtype_t)PMX_ASSET_SUBTYPE, "assets");
    if (assets == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "PMX assets partition not found");
        return ESP_FAIL;
    }

    if (request->content_len < (int)sizeof(pmx_header_t) ||
        request->content_len > (int)assets->size) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid PMX file");
        return ESP_FAIL;
    }

    pmx_header_t header;
    if (!http_receive_exact(request, (char *)&header, sizeof(header)) ||
        !pmx_header_is_valid(&header, assets->size, (size_t)request->content_len)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid PMX header");
        return ESP_FAIL;
    }

    s_http_uploading = true;
    s_assets_updating = true;
    ++s_pmx_generation;
    vTaskDelay(pdMS_TO_TICKS(30));
    pmx_release_current_cache();
    s_display_paused = true;
    vTaskDelay(pdMS_TO_TICKS(2));

    const size_t erase_size = ((size_t)request->content_len + 4095U) & ~4095U;
    if (erase_size > assets->size ||
        esp_partition_erase_range(assets, 0, erase_size) != ESP_OK ||
        esp_partition_write(assets, 0, &header, sizeof(header)) != ESP_OK) {
        s_display_paused = false;
        s_assets_updating = false;
        s_http_uploading = false;
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
        return ESP_FAIL;
    }

    char buffer[1024];
    size_t offset = sizeof(header);
    while (offset < (size_t)request->content_len) {
        const size_t remaining = (size_t)request->content_len - offset;
        const size_t block_size = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        if (!http_receive_exact(request, buffer, block_size) ||
            esp_partition_write(assets, offset, buffer, block_size) != ESP_OK) {
            s_display_paused = false;
            s_assets_updating = false;
            s_http_uploading = false;
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "PMX transfer failed");
            return ESP_FAIL;
        }
        offset += block_size;
    }

    s_display_paused = false;
    ++s_pmx_generation;
    s_assets_updating = false;
    s_http_uploading = false;
    ESP_LOGI(TAG, "HTTP Flash upload complete: %d bytes, %lu frames; PMX generation %lu",
             request->content_len, (unsigned long)header.frame_count, (unsigned long)s_pmx_generation);
    httpd_resp_sendstr(request, "Flash upload complete. Display will switch to the saved PMX.");
    return ESP_OK;
}

static const httpd_uri_t s_root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = http_root_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t s_ping_uri = {
    .uri = "/ping",
    .method = HTTP_GET,
    .handler = http_ping_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t s_upload_uri = {
    .uri = "/upload",
    .method = HTTP_POST,
    .handler = http_upload_handler,
    .user_ctx = NULL,
};

static void start_web_server(void)
{
    if (s_http_server != NULL) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 6144;
    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_root_uri));
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_ping_uri));
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_upload_uri));
        ESP_LOGI(TAG, "HTTP PMX upload server started");
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected. Open http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        start_web_server();
    }
}

static void wifi_init(void)
{
    if (strcmp(MATRIX_WIFI_SSID, "YOUR_2_4G_WIFI_NAME") == 0 ||
        strcmp(MATRIX_WIFI_PASSWORD, "YOUR_WIFI_PASSWORD") == 0) {
        ESP_LOGW(TAG, "Set MATRIX_WIFI_SSID and MATRIX_WIFI_PASSWORD in wifi_config.h to enable Wi-Fi upload");
        return;
    }

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, MATRIX_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, MATRIX_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void pmx_player_task(void *argument)
{
    (void)argument;
    const esp_partition_t *assets = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        (esp_partition_subtype_t)PMX_ASSET_SUBTYPE, "assets");
    for (;;) {
        if (pmx_adopt_pending_cache()) {
            /* Continue into the RAM playback path below. */
        }

        if (s_cached_frames != NULL && s_cached_frame_count > 0) {
            const uint32_t loaded_generation = s_pmx_generation;
            const uint32_t playback_frame_count = s_cached_frame_count;
            ESP_LOGI(TAG, "Playing RAM PMX: %lu frames",
                     (unsigned long)playback_frame_count);
            if (playback_frame_count == 1) {
                pmx_show_cached_frame(0);
                while (!s_pending_cache_ready && s_pmx_generation == loaded_generation) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                continue;
            }
            for (uint32_t frame_index = 0;
                 !s_pending_cache_ready && s_pmx_generation == loaded_generation;
                 frame_index = (frame_index + 1) % playback_frame_count) {
                pmx_show_cached_frame(frame_index);
                vTaskDelay(pdMS_TO_TICKS(s_cached_delays[frame_index]));
            }
            continue;
        }

        pmx_header_t header;
        if (s_assets_updating) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!pmx_read_header(assets, &header)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const uint32_t loaded_generation = s_pmx_generation;

        ESP_LOGI(TAG, "Loading PMX from flash assets: %lu frames, %lu bytes",
                 (unsigned long)header.frame_count, (unsigned long)header.file_size);

        if (pmx_cache_frames(assets, &header)) {
            continue;
        }

        if (header.frame_count == 1) {
            uint32_t delay_ms = 100;
            if (pmx_load_frame(assets, &header, 0, &delay_ms)) {
                while (!s_assets_updating && !s_pending_cache_ready &&
                       s_pmx_generation == loaded_generation) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                continue;
            }
        }

        for (uint32_t frame_index = 0;
             !s_assets_updating && !s_pending_cache_ready && s_pmx_generation == loaded_generation;
             frame_index = (frame_index + 1) % header.frame_count) {
            uint32_t delay_ms = 100;
            if (!pmx_load_frame(assets, &header, frame_index, &delay_ms)) {
                ESP_LOGE(TAG, "Unable to load PMX frame %lu", (unsigned long)frame_index);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
}

void app_main(void)
{
    s_cache_mutex = xSemaphoreCreateMutex();
    if (s_cache_mutex == NULL) {
        ESP_LOGE(TAG, "Unable to create PMX cache mutex");
        return;
    }
    hub75_init();
    show_builtin_test_pattern();
    xTaskCreatePinnedToCore(hub75_refresh_task, "hub75_refresh", 4096, NULL,
                            configMAX_PRIORITIES - 2, NULL, 1);
    xTaskCreatePinnedToCore(pmx_player_task, "pmx_player", 4096, NULL, 5, NULL, 0);
    wifi_init();
}
