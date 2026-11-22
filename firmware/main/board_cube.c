// Waveshare ESP32-S3-Touch-AMOLED-2.16 ("cube"): 480x480 AMOLED (CO5300), CST9220 touch, AXP2101 PMU.
// Buttons: KEY3 = GPIO18, BOOT = GPIO0, PWR = AXP2101 power key (read via its IRQ registers).
#include "sdkconfig.h"
#if CONFIG_KP_BOARD_CUBE
#include "board.h"
#include "config.h"
#include "settings.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lv_adapter.h"
#include "esp_lcd_touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>

static const char *TAG = "cube";
static i2c_master_dev_handle_t s_pmu;

#define PMU_ADDR 0x34
#define GPIO_KEY3 GPIO_NUM_18
#define GPIO_BOOT GPIO_NUM_0

// Draw buffers live in internal RAM: the SPI driver can send them directly. In PSRAM (the BSP default)
// rendering is slower and the driver copies every strip into a fresh internal buffer before sending it.
// 2 x 24 lines = 45 KB, allocated once at boot; that is less than the copy the driver used to allocate per strip.
#define DRAW_LINES 24

// ---------------------------------------------------------------- touch (CST92xx)
// Read here instead of through the Waveshare driver: it never acknowledges a report, so current CST92xx
// firmware holds each report until its own timeout, and when a read fails it sleeps and resets the
// controller inside the LVGL task. The protocol follows Espressif's esp_lcd_touch_cst9220 driver.
// Touch is polled every LVGL input period (16 ms) instead of waiting for the interrupt pin: esp_lvgl_adapter
// 0.6.x skips a sample it could not read and then keeps reporting the old finger position.

#define TOUCH_ADDR 0x5A
#define TOUCH_REPORT_LEN 15            // up to 2 points x 5 bytes + 5 header bytes
#define TOUCH_PROJECT_LEGACY 0x542F    // older firmware that must not get the acknowledgement
#define TOUCH_STALE_US 300000          // a finger on the glass keeps producing reports; this much silence = gone

static i2c_master_dev_handle_t s_touch;
static bool s_touch_ack = true;
static int64_t s_touch_last_report;
static uint32_t s_touch_reports, s_touch_errors, s_touch_stale;

static esp_err_t touch_read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    const uint8_t r[2] = {reg >> 8, reg & 0xFF};
    esp_err_t err = i2c_master_transmit(s_touch, r, sizeof(r), 20);
    return err == ESP_OK ? i2c_master_receive(s_touch, buf, len, 20) : err;
}

// esp_lcd_touch read_data: decode one report into tp->data. Always returns ESP_OK; errors keep the last state.
static esp_err_t touch_read(esp_lcd_touch_handle_t tp)
{
    uint8_t r[TOUCH_REPORT_LEN] = {0};
    int points = -1;  // -1: no new report
    uint16_t x = 0, y = 0;
    if (touch_read_reg(0xD000, r, sizeof(r)) != ESP_OK) {
        s_touch_errors++;
    } else {
        if (s_touch_ack) {  // releases the report so the controller can publish the next one
            static const uint8_t ack[3] = {0xD0, 0x00, 0xAB};
            if (i2c_master_transmit(s_touch, ack, sizeof(ack), 20) != ESP_OK) s_touch_errors++;
        }
        bool empty = true;
        for (int i = 0; i < 7; i++) empty = empty && r[i] == 0;
        int n = r[5] & 0x7F;
        if (empty) {
            points = 0;
        } else if (r[0] != 0xAB && r[6] == 0xAB && n <= 2) {  // r[0] == 0xAB: nothing new since the last one
            points = 0;
            for (int i = 0; i < n; i++) {
                const uint8_t *p = &r[i == 0 ? 0 : i * 5 + 2];
                if (((p[0] & 0x0F) >> 1) == 3) {  // release records keep the old position, so filter on status
                    x = (uint16_t)(p[1] << 4 | p[3] >> 4);
                    y = (uint16_t)(p[2] << 4 | (p[3] & 0x0F));
                    points = 1;
                    break;
                }
            }
        }
    }
    int64_t now = esp_timer_get_time();
    if (points >= 0) {
        s_touch_reports++;
        s_touch_last_report = now;
    }
    portENTER_CRITICAL(&tp->data.lock);
    if (points >= 0) {
        tp->data.points = points;
        if (points) {
            tp->data.coords[0].x = x;
            tp->data.coords[0].y = y;
            tp->data.coords[0].strength = 1;
            tp->data.coords[0].track_id = 0;
        }
    } else if (tp->data.points && now - s_touch_last_report > TOUCH_STALE_US) {
        tp->data.points = 0;  // never hold a finger that has stopped reporting
        s_touch_stale++;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static void touch_start(lv_display_t *disp)
{
    // Same orientation as bsp_display_start(). The Waveshare driver still does the reset and setup.
    bsp_display_cfg_t cfg = {.touch_flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1}};
    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(&cfg, &tp) != ESP_OK || !tp) {
        ESP_LOGW(TAG, "touch not available");
        return;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &s_touch) == ESP_OK) {
        uint8_t id[4] = {0};
        if (touch_read_reg(0xD204, id, sizeof(id)) == ESP_OK) {
            uint16_t project = id[0] | id[1] << 8, chip = id[2] | id[3] << 8;
            s_touch_ack = project != TOUCH_PROJECT_LEGACY;
            ESP_LOGI(TAG, "touch chip 0x%04x, project 0x%04x", chip, project);
        }
        tp->read_data = touch_read;
        gpio_intr_disable(tp->config.int_gpio_num);
        tp->config.int_gpio_num = GPIO_NUM_NC;  // the adapter polls when there is no interrupt pin
    } else {
        s_touch = NULL;
        ESP_LOGW(TAG, "touch: using the Waveshare driver");
    }
    esp_lv_adapter_touch_config_t tc = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    if (!esp_lv_adapter_register_touch(&tc)) ESP_LOGW(TAG, "touch registration failed");
}

// The CO5300 needs areas that start on an even and end on an odd pixel (as in the BSP).
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

// bsp_display_start() with three changes: internal draw buffers, the LVGL task on core 1 (Wi-Fi, TLS and
// cover decoding run on core 0), and our own touch reading.
static lv_display_t *display_start(void)
{
    esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    acfg.task_core_id = 1;
    if (esp_lv_adapter_init(&acfg) != ESP_OK) return NULL;
    const bsp_display_config_t pcfg = {.max_transfer_sz = KP_SCREEN_W * KP_SCREEN_H * 2};
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (bsp_display_new(&pcfg, &panel, &io) != ESP_OK) return NULL;
    esp_lv_adapter_display_config_t dcfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = KP_SCREEN_W,
            .ver_res = KP_SCREEN_H,
            .buffer_height = DRAW_LINES,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&dcfg);
    if (!disp) {  // slower, but better than no screen
        ESP_LOGW(TAG, "no internal RAM for draw buffers, using PSRAM");
        dcfg.profile.use_psram = true;
        disp = esp_lv_adapter_register_display(&dcfg);
    }
    if (!disp) return NULL;
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    touch_start(disp);
    bsp_display_brightness_init();
    if (esp_lv_adapter_start() != ESP_OK) return NULL;
    return disp;
}

static esp_err_t pmu_read(uint8_t reg, uint8_t *val)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_pmu, &reg, 1, val, 1, 100);
}

static esp_err_t pmu_write(uint8_t reg, uint8_t val)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_pmu, b, 2, 100);
}

static void pmu_set_bits(uint8_t reg, uint8_t bits)
{
    uint8_t v;
    if (pmu_read(reg, &v) == ESP_OK) pmu_write(reg, v | bits);
}

static void pmu_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMU_ADDR,
        .scl_speed_hz = 400000,
    };
    if (!bus || i2c_master_bus_add_device(bus, &cfg, &s_pmu) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not available");
        s_pmu = NULL;
        return;
    }
    uint8_t id = 0;
    pmu_read(0x03, &id);
    ESP_LOGI(TAG, "AXP2101 chip id 0x%02x", id);
    pmu_set_bits(0x68, 0x01);  // battery detection on
    pmu_set_bits(0x30, 0x01);  // battery voltage ADC on
    pmu_set_bits(0x18, 0x08);  // fuel gauge on
    pmu_write(0x48, 0xFF);     // clear pending IRQs
    pmu_write(0x49, 0xFF);
    pmu_write(0x4A, 0xFF);
    pmu_set_bits(0x41, 0x0C);  // power key short (bit3) and long (bit2) press IRQs
}

lv_display_t *board_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_KEY3) | (1ULL << GPIO_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    bsp_i2c_init();
    pmu_init();
    lv_display_t *disp = display_start();
    if (disp) bsp_display_brightness_set(100);
    return disp;
}

void board_diag(char *buf, size_t len)
{
    if (!s_touch) {
        snprintf(buf, len, "touch: Waveshare driver");
        return;
    }
    snprintf(buf, len, "touch: %lu reports, %lu read errors, %lu stuck presses cleared%s",
             (unsigned long)s_touch_reports, (unsigned long)s_touch_errors, (unsigned long)s_touch_stale,
             s_touch_ack ? "" : " (older touch firmware)");
}

// The adapter's lock is recursive. Call it directly: bsp_display_lock(0) does not wait (0 = try once in
// esp_lvgl_adapter) and its bool result is ESP_OK cast to false.
bool board_lock(int timeout_ms) { return esp_lv_adapter_lock(timeout_ms < 0 ? -1 : timeout_ms) == ESP_OK; }
void board_unlock(void) { esp_lv_adapter_unlock(); }

void board_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (board_lock(500)) {
        bsp_display_brightness_set(percent);
        board_unlock();
    }
}

void board_apply_rotation(void) {}

typedef struct {
    bool down;
    int64_t since;
    bool long_sent;
} key_t_;

static key_t_ s_key3, s_boot;
static int s_poll;

// Returns 1 on release after a short press, 2 once when held for long_ms.
static int key_update(key_t_ *k, bool pressed, int long_ms)
{
    int64_t now = esp_timer_get_time();
    if (pressed && !k->down) {
        k->down = true;
        k->since = now;
        k->long_sent = false;
    } else if (pressed && k->down && !k->long_sent && long_ms > 0 && now - k->since > (int64_t)long_ms * 1000) {
        k->long_sent = true;
        return 2;
    } else if (!pressed && k->down) {
        k->down = false;
        if (!k->long_sent && now - k->since > 30000) return 1;  // >30 ms = debounced click
    }
    return 0;
}

board_button_t board_poll_button(void)
{
    bool swap = g_set.swap_buttons != 0;
    int k3 = key_update(&s_key3, gpio_get_level(GPIO_KEY3) == 0, 0);
    if (k3 == 1) return swap ? BTN_VOL_DOWN : BTN_VOL_UP;
    int kb = key_update(&s_boot, gpio_get_level(GPIO_BOOT) == 0, 3000);
    if (kb == 2) return BTN_PARENT;
    if (kb == 1) return swap ? BTN_VOL_UP : BTN_VOL_DOWN;
    if (++s_poll >= 5) {  // PMU every ~100 ms
        s_poll = 0;
        uint8_t st = 0;
        if (pmu_read(0x49, &st) == ESP_OK && (st & 0x0C)) {
            pmu_write(0x49, st & 0x0C);
            if (st & 0x08) return BTN_PLAY_PAUSE;  // short press
        }
    }
    return BTN_NONE;
}

int board_battery(bool *charging)
{
    uint8_t s0 = 0, s1 = 0, pct = 0;
    if (pmu_read(0x00, &s0) != ESP_OK || !(s0 & 0x08)) return -1;  // no battery
    if (charging && pmu_read(0x01, &s1) == ESP_OK) *charging = ((s1 >> 5) & 0x03) == 0x01;
    if (pmu_read(0xA4, &pct) != ESP_OK || pct > 100) return -1;
    return pct;
}

void board_power_off(void)
{
    pmu_set_bits(0x10, 0x01);  // AXP2101: power off
}
#endif
