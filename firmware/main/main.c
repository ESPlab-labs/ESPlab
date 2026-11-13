/* ESPlab firmware main — thin adapter: real radio in, real serial out,
 * the same tested core doing all the thinking.
 *
 * Flow: ESP-NOW datagrams -> gl_recv(); gl_tick() every loop;
 * UART lines -> gl_console(); console output -> UART.
 * MAC address is the board's ESPlab id — real hardware addresses. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "machine.h"

static gl_node_t node;
static gl_transport_t bus;

static uint64_t now_ms(void)
{
    return (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- transport: ESP-NOW ---- */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    /* hand straight to the core; it does its own validation */
    gl_recv(&node, data, (size_t)len);
}

static int now_send(gl_transport_t *t, const uint8_t peer[6],
                    const uint8_t *payload, size_t len)
{
    (void)t;
    if (len > 250) return -1;          /* the core fragments above this */
    if (!esp_now_is_peer_exist(peer)) {
        esp_now_peer_info_t pi = { 0 };
        memcpy(pi.peer_addr, peer, 6);
        pi.channel = 0;
        pi.encrypt = false;
        if (esp_now_add_peer(&pi) != ESP_OK) return -1;
    }
    return (esp_now_send(peer, payload, (int)len) == ESP_OK) ? 0 : -1;
}

static int now_broadcast(gl_transport_t *t, const uint8_t *payload, size_t len)
{
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    return now_send(t, bcast, payload, len);
}

/* ---- console: UART0, the USB cable ---- */

/* console output sink (declared before the console task that uses it) */
void uart_print(void *ctx, const char *text);

static void uart_console_task(void *arg)
{
    /* frame submits carry up to ~5.6KB of hex (a 2.7KB RLE frame) —
     * must match the host console buffer (11000) */
    static uint8_t line[11000];
    size_t n = 0;
    for (;;) {
        /* bulk-read: one byte per loop with a delay would turn a 5.6KB
         * frame submit into minutes of 5ms-per-byte trickle */
        uint8_t tmp[256];
        int got = uart_read_bytes(UART_NUM_0, tmp, sizeof(tmp),
                                  pdMS_TO_TICKS(20));
        for (int i = 0; i < got; i++) {
            uint8_t c = tmp[i];
            if (c == '\n' || c == '\r') {
                if (n) {
                    line[n] = 0;
                    gl_console(&node, (const char *)line, uart_print, NULL);
                    n = 0;
                }
            } else if (n < sizeof(line) - 1) {
                line[n++] = c;
            } else {
                uart_print(NULL, "console: line too long");
                n = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void uart_print(void *ctx, const char *text)
{
    (void)ctx;
    uart_write_bytes(UART_NUM_0, text, (int)strlen(text));
    uart_write_bytes(UART_NUM_0, "\r\n", 2);
}

/* ---- app_main: bring up NVS, WiFi(radio), ESP-NOW, the core ---- */
void app_main(void)
{
    /* console first so early messages are visible. RX ring must hold a
     * full frame submit (~5.6KB hex) arriving in one burst; 921600 baud
     * keeps a frame submit well under a second on the wire. */
    uart_driver_install(UART_NUM_0, 12288, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0,
        &(uart_config_t){ .baud_rate = 921600, .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT });
    uart_print(NULL, "ESPlab booting...");

    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    /* the core: this board's MAC is its id */
    bus.now_ms = now_ms;
    bus.send = now_send;
    bus.broadcast = now_broadcast;
    bus.ctx = &bus;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    gl_init(&node, &bus, mac, ELP_CHIP_CLASSIC, 2, 290, 4, 0, 0);
    uart_print(NULL, "ESPlab node up");

    xTaskCreate(uart_console_task, "console", 4096, NULL, 5, NULL);

    /* main loop: the core breathes here */
    for (;;) {
        bus.now_ms = now_ms();
        gl_tick(&node);
        vTaskDelay(pdMS_TO_TICKS(10));   /* 100 ticks/sec, like the sim */
    }
}
