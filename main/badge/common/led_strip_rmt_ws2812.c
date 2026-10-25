// Copyright 2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "led_strip.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "ws2812";
#define STRIP_CHECK(a, str, goto_tag, ret_value, ...)                             \
    do                                                                            \
    {                                                                             \
        if (!(a))                                                                 \
        {                                                                         \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            ret = ret_value;                                                      \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

// RMT tick = 25 ns (40 MHz)
#define WS2812_RMT_RESOLUTION_HZ (40 * 1000 * 1000)
#define WS2812_NS_TO_TICKS(ns)   ((ns) * (WS2812_RMT_RESOLUTION_HZ / 1000000) / 1000)

#define WS2812_T0H_NS (350)
#define WS2812_T0L_NS (1000)
#define WS2812_T1H_NS (800)
#define WS2812_T1L_NS (600)

typedef struct {
    led_strip_t parent;
    rmt_channel_handle_t rmt_channel;
    rmt_encoder_handle_t encoder;
    uint32_t strip_len;
    uint8_t buffer[0];
} ws2812_t;

static esp_err_t ws2812_set_pixel(led_strip_t *strip, uint32_t index, uint32_t red, uint32_t green, uint32_t blue)
{
    esp_err_t ret = ESP_OK;
    ws2812_t *ws2812 = __containerof(strip, ws2812_t, parent);
    STRIP_CHECK(index < ws2812->strip_len, "index out of the maximum number of leds", err, ESP_ERR_INVALID_ARG);
    uint32_t start = index * 3;
    // In thr order of GRB
    ws2812->buffer[start + 0] = green & 0xFF;
    ws2812->buffer[start + 1] = red & 0xFF;
    ws2812->buffer[start + 2] = blue & 0xFF;
    return ESP_OK;
err:
    return ret;
}

static esp_err_t ws2812_refresh(led_strip_t *strip, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ws2812_t *ws2812 = __containerof(strip, ws2812_t, parent);
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    STRIP_CHECK(rmt_transmit(ws2812->rmt_channel, ws2812->encoder, ws2812->buffer, ws2812->strip_len * 3, &tx_config) == ESP_OK,
                "transmit RMT samples failed", err, ESP_FAIL);
    return rmt_tx_wait_all_done(ws2812->rmt_channel, timeout_ms);
err:
    return ret;
}

static esp_err_t ws2812_clear(led_strip_t *strip, uint32_t timeout_ms)
{
    ws2812_t *ws2812 = __containerof(strip, ws2812_t, parent);
    // Write zero to turn off all leds
    memset(ws2812->buffer, 0, ws2812->strip_len * 3);
    return ws2812_refresh(strip, timeout_ms);
}

static esp_err_t ws2812_del(led_strip_t *strip)
{
    ws2812_t *ws2812 = __containerof(strip, ws2812_t, parent);
    rmt_disable(ws2812->rmt_channel);
    rmt_del_encoder(ws2812->encoder);
    rmt_del_channel(ws2812->rmt_channel);
    free(ws2812);
    return ESP_OK;
}

led_strip_t *led_strip_new_rmt_ws2812(const led_strip_config_t *config)
{
    led_strip_t *ret = NULL;
    STRIP_CHECK(config, "configuration can't be null", err, NULL);

    // 24 bits per led
    uint32_t ws2812_size = sizeof(ws2812_t) + config->max_leds * 3;
    ws2812_t *ws2812 = calloc(1, ws2812_size);
    STRIP_CHECK(ws2812, "request memory for ws2812 failed", err, NULL);

    rmt_tx_channel_config_t tx_config = {
        .gpio_num = config->gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 48,  // one C3 RMT block
        .trans_queue_depth = 4,
    };
    STRIP_CHECK(rmt_new_tx_channel(&tx_config, &ws2812->rmt_channel) == ESP_OK,
                "create RMT TX channel failed", err_free, NULL);

    // each data bit is one RMT symbol, MSB first; the line idles low between frames (reset)
    rmt_bytes_encoder_config_t enc_config = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_NS_TO_TICKS(WS2812_T0H_NS),
                  .level1 = 0, .duration1 = WS2812_NS_TO_TICKS(WS2812_T0L_NS) },
        .bit1 = { .level0 = 1, .duration0 = WS2812_NS_TO_TICKS(WS2812_T1H_NS),
                  .level1 = 0, .duration1 = WS2812_NS_TO_TICKS(WS2812_T1L_NS) },
        .flags.msb_first = 1,
    };
    STRIP_CHECK(rmt_new_bytes_encoder(&enc_config, &ws2812->encoder) == ESP_OK,
                "create RMT encoder failed", err_chan, NULL);
    STRIP_CHECK(rmt_enable(ws2812->rmt_channel) == ESP_OK,
                "enable RMT channel failed", err_enc, NULL);

    ws2812->strip_len = config->max_leds;

    ws2812->parent.set_pixel = ws2812_set_pixel;
    ws2812->parent.refresh = ws2812_refresh;
    ws2812->parent.clear = ws2812_clear;
    ws2812->parent.del = ws2812_del;

    return &ws2812->parent;
err_enc:
    rmt_del_encoder(ws2812->encoder);
err_chan:
    rmt_del_channel(ws2812->rmt_channel);
err_free:
    free(ws2812);
err:
    return ret;
}

led_strip_t * led_strip_init(gpio_num_t gpio, uint16_t led_num)
{
    // install ws2812 driver
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(led_num, gpio);

    led_strip_t *pStrip = led_strip_new_rmt_ws2812(&strip_config);

    if ( !pStrip ) {
        ESP_LOGE(TAG, "install WS2812 driver failed");
        return NULL;
    }

    // Clear LED strip (turn off all LEDs)
    ESP_ERROR_CHECK(pStrip->clear(pStrip, 100));

    return pStrip;
}

esp_err_t led_strip_denit(led_strip_t *strip)
{
    return strip->del(strip);
}
