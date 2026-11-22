// Waveshare ESP32-S3-Touch-AMOLED-2.16 ("cube"): 480x480 AMOLED (CO5300), CST9220 touch, AXP2101 PMU.
// Buttons: KEY3 = GPIO18, BOOT = GPIO0, PWR = AXP2101 power key (read via its IRQ registers).
#include "sdkconfig.h"
#if CONFIG_KP_BOARD_CUBE
#include "board.h"
#include "config.h"
#include "cst92xx.h"
#include "settings.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "cube";
static i2c_master_dev_handle_t s_pmu;

#define PMU_ADDR 0x34
#define GPIO_KEY3 GPIO_NUM_18
#define GPIO_BOOT GPIO_NUM_0

// ---------------------------------------------------------------- touch (CST92xx)
// Read here instead of through the Waveshare driver: it never acknowledges a report, so current CST92xx
// firmware holds each report until its own timeout, and when a read fails it sleeps and resets the
// controller inside the LVGL task. Decoding and fault handling are in cst92xx.c.
// Our own LVGL input device, polled every input period (16 ms) instead of waiting for the interrupt pin:
// only the input device can end a press without LVGL counting it as a tap.

#define TOUCH_ADDR 0x5A
#define TOUCH_PROJECT_LEGACY 0x542F    // older firmware that must not get the acknowledgement

static esp_lcd_touch_handle_t s_tp;
static i2c_master_dev_handle_t s_touch;
static bool s_touch_ack = true;
static cst_tracker_t s_tracker;
static uint32_t s_touch_reports, s_touch_failed, s_touch_cancelled;

static esp_err_t touch_read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    const uint8_t r[2] = {reg >> 8, reg & 0xFF};
    esp_err_t err = i2c_master_transmit(s_touch, r, sizeof(r), 20);
    return err == ESP_OK ? i2c_master_receive(s_touch, buf, len, 20) : err;
}

static void touch_indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t r[CST92XX_REPORT_LEN] = {0};
    bool read_ok = touch_read_reg(0xD000, r, sizeof(r)) == ESP_OK, ack_ok = true;
    if (read_ok && s_touch_ack) {  // releases the report so the controller can publish the next one
        static const uint8_t ack[3] = {0xD0, 0x00, 0xAB};
        ack_ok = i2c_master_transmit(s_touch, ack, sizeof(ack), 20) == ESP_OK;
    }
    cst_sample_t smp = cst92xx_decode(r, read_ok, ack_ok);
    if (smp.kind == CST_SAMPLE_FAILED) s_touch_failed++;
    else if (smp.kind != CST_SAMPLE_NO_NEWS) s_touch_reports++;

    data->state = LV_INDEV_STATE_RELEASED;
    switch (cst92xx_track(&s_tracker, smp, esp_timer_get_time())) {
    case CST_OUT_DOWN:
        data->state = LV_INDEV_STATE_PRESSED;
        break;
    case CST_OUT_CANCEL:
        s_touch_cancelled++;
        // A drag ends as if the finger were lifted: the cards snap into place, and LVGL never counts a drag as
        // a tap. Anything else gets PRESS_LOST instead of RELEASED and CLICKED.
        if (!lv_indev_get_scroll_obj(indev)) lv_indev_wait_release(indev);
        break;
    case CST_OUT_UP:
        break;
    }
    // Controller to screen as bsp_display_start() configured it (mirror y, then swap). Clamped: LVGL logs a
    // warning over the slow UART for every point outside the screen.
    int sx = KP_SCREEN_H - s_tracker.y, sy = s_tracker.x;
    data->point.x = sx < 0 ? 0 : sx >= KP_SCREEN_W ? KP_SCREEN_W - 1 : sx;
    data->point.y = sy < 0 ? 0 : sy >= KP_SCREEN_H ? KP_SCREEN_H - 1 : sy;
}

// Only if our own I2C handle could not be added: the Waveshare driver, polled.
static void touch_fallback_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_point_data_t p[1];
    uint8_t n = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    esp_lcd_touch_read_data(s_tp);
    if (esp_lcd_touch_get_data(s_tp, p, &n, 1) == ESP_OK && n) {
        data->point.x = p[0].x < KP_SCREEN_W ? p[0].x : KP_SCREEN_W - 1;
        data->point.y = p[0].y < KP_SCREEN_H ? p[0].y : KP_SCREEN_H - 1;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

static void touch_start(lv_display_t *disp)
{
    // The Waveshare driver still resets and sets up the controller.
    bsp_display_cfg_t cfg = {.touch_flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1}};
    if (bsp_touch_new(&cfg, &s_tp) != ESP_OK || !s_tp) {
        ESP_LOGW(TAG, "touch not available");
        return;
    }
    gpio_intr_disable(s_tp->config.int_gpio_num);  // polled, see above
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    lv_indev_read_cb_t read = touch_indev_read;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &s_touch) != ESP_OK) {
        s_touch = NULL;
        read = touch_fallback_read;
        ESP_LOGW(TAG, "touch: using the Waveshare driver");
    } else {
        uint8_t id[4] = {0};
        if (touch_read_reg(0xD204, id, sizeof(id)) == ESP_OK) {
            uint16_t project = id[0] | id[1] << 8, chip = id[2] | id[3] << 8;
            s_touch_ack = project != TOUCH_PROJECT_LEGACY;
            ESP_LOGI(TAG, "touch chip 0x%04x, project 0x%04x", chip, project);
        }
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read);
    lv_indev_set_display(indev, disp);
}

// ---------------------------------------------------------------- display (CO5300 over QSPI)
// This board doesn't connect the panel's tearing-effect (TE) pin, so writes can't be timed to the panel's
// refresh: whenever the panel shows its memory while a frame is half written, it shows two frames at once.
// Drawing in horizontal strips (the BSP way) made that a horizontal cut through every cover, and the cut
// crept down the screen for the ~40 ms it took to draw a frame. Instead LVGL draws whole frames into two PSRAM
// buffers, and a sender task pushes each finished frame to the panel in vertical slices, left to right, while
// LVGL already draws the next one. A half-written frame then shows as a vertical seam, which hardly shows
// while the cards move sideways, and it lasts only as long as the transfer.
// LVGL runs without its OS layer on this board (sdkconfig.defaults.cube): otherwise it draws in a separate
// priority-3 task with no core affinity that competes with cover downloads and decoding.

#define SLICE_W 16                           // columns per transfer; x must stay even and x + SLICE_W - 1 odd
#define SLICE_BYTES (SLICE_W * KP_SCREEN_H * 2)
#define FRAME_BYTES (KP_SCREEN_W * KP_SCREEN_H * 2)

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static SemaphoreHandle_t s_lvgl_mutex;   // LVGL: board_lock()
static SemaphoreHandle_t s_panel_mutex;  // SPI commands: a frame being sent, or a brightness change
static SemaphoreHandle_t s_slice_free;   // counts free slice buffers; given back when a transfer ends
static SemaphoreHandle_t s_frame_sent;   // one per flushed frame, taken by LVGL's flush-wait callback
static TaskHandle_t s_sender;
static const uint16_t *volatile s_frame;
static uint16_t *s_slice[2];
static volatile bool s_first_frame_sent;
static int s_brightness = 100;
static uint32_t s_frames_sent, s_frame_us_total, s_frame_us_max, s_send_errors;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_slice_free, &woken);
    return woken == pdTRUE;
}

// Copy columns x0..x0+SLICE_W-1 into a contiguous buffer, swapping to the panel's big-endian RGB565.
static void gather(uint32_t *dst, const uint16_t *frame, int x0)
{
    for (int y = 0; y < KP_SCREEN_H; y++) {
        const uint32_t *src = (const uint32_t *)(frame + y * KP_SCREEN_W + x0);
        for (int i = 0; i < SLICE_W / 2; i++) {
            uint32_t w = src[i];
            *dst++ = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
        }
    }
}

static void sender_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint16_t *frame = s_frame;
        int64_t t0 = esp_timer_get_time();
        xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
        // Two slice buffers: fill one while the other is on the wire. Transfers finish in order, so each
        // free count given back belongs to the buffer that is filled next.
        for (int x = 0, i = 0; x < KP_SCREEN_W; x += SLICE_W, i ^= 1) {
            xSemaphoreTake(s_slice_free, portMAX_DELAY);
            gather((uint32_t *)s_slice[i], frame, x);
            if (esp_lcd_panel_draw_bitmap(s_panel, x, 0, x + SLICE_W, KP_SCREEN_H, s_slice[i]) != ESP_OK) {
                s_send_errors++;
                xSemaphoreGive(s_slice_free);  // no transfer, so no completion will give it back
            }
        }
        xSemaphoreTake(s_slice_free, portMAX_DELAY);  // both slices back: the frame is on the panel
        xSemaphoreTake(s_slice_free, portMAX_DELAY);
        xSemaphoreGive(s_slice_free);
        xSemaphoreGive(s_slice_free);
        if (!s_first_frame_sent) {  // the panel showed old memory until now
            s_first_frame_sent = true;
            bsp_display_brightness_set(s_brightness);
        }
        xSemaphoreGive(s_panel_mutex);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        s_frames_sent++;
        s_frame_us_total += us;
        if (us > s_frame_us_max) s_frame_us_max = us;
        xSemaphoreGive(s_frame_sent);
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    s_frame = (const uint16_t *)px;  // full render mode: always the whole screen
    xTaskNotifyGive(s_sender);
}

// LVGL calls this once per flushed frame before it reuses that frame's buffer, and then clears its
// flushing flag itself (so the sender never calls lv_display_flush_ready).
static void flush_wait_cb(lv_display_t *disp) { xSemaphoreTake(s_frame_sent, portMAX_DELAY); }

static uint32_t tick_get(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t delay = 5;
        if (xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            delay = lv_timer_handler();
            xSemaphoreGiveRecursive(s_lvgl_mutex);
        }
        if (delay < 1) delay = 1;
        if (delay > 20) delay = 20;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static lv_display_t *display_start(void)
{
    const bsp_display_config_t pcfg = {.max_transfer_sz = SLICE_BYTES};
    esp_lcd_panel_io_handle_t io = NULL;
    if (bsp_display_new(&pcfg, &s_panel, &io) != ESP_OK) return NULL;
    bsp_display_brightness_set(0);  // dark until the first frame replaces whatever the panel memory holds

    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    s_panel_mutex = xSemaphoreCreateMutex();
    s_slice_free = xSemaphoreCreateCounting(2, 2);
    s_frame_sent = xSemaphoreCreateBinary();
    s_slice[0] = heap_caps_malloc(SLICE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_slice[1] = heap_caps_malloc(SLICE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *fb1 = heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    void *fb2 = heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_lvgl_mutex || !s_panel_mutex || !s_slice_free || !s_frame_sent || !s_slice[0] || !s_slice[1] || !fb1 ||
        !fb2) {
        ESP_LOGE(TAG, "display: out of memory");
        return NULL;
    }
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_color_done};
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);

    lv_init();
    lv_tick_set_cb(tick_get);
    s_disp = lv_display_create(KP_SCREEN_W, KP_SCREEN_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, fb1, fb2, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    touch_start(s_disp);

    xTaskCreatePinnedToCore(sender_task, "lcd_send", 4096, NULL, 6, &s_sender, 0);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "display ready");
    return s_disp;
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
    return display_start();
}

void board_diag(char *buf, size_t len)
{
    int n = snprintf(buf, len, "frames sent: %lu, average %lu ms, slowest %lu ms%s · ", (unsigned long)s_frames_sent,
                     (unsigned long)(s_frames_sent ? s_frame_us_total / s_frames_sent / 1000 : 0),
                     (unsigned long)(s_frame_us_max / 1000), s_send_errors ? ", send errors" : "");
    if (n < 0 || (size_t)n >= len) return;
    if (!s_touch) {
        snprintf(buf + n, len - n, "touch: Waveshare driver");
        return;
    }
    snprintf(buf + n, len - n, "touch: %lu reports, %lu failed reads, %lu presses cancelled%s",
             (unsigned long)s_touch_reports, (unsigned long)s_touch_failed, (unsigned long)s_touch_cancelled,
             s_touch_ack ? "" : " (older touch firmware)");
}

bool board_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, t) == pdTRUE;
}

void board_unlock(void) { xSemaphoreGiveRecursive(s_lvgl_mutex); }

void board_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    // Not in the middle of a frame (the sender holds the panel for one). Before the first frame the sender
    // applies s_brightness itself.
    if (s_first_frame_sent && xSemaphoreTake(s_panel_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        bsp_display_brightness_set(percent);
        xSemaphoreGive(s_panel_mutex);
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
