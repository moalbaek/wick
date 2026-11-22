// CST92xx touch reports (the cube's touch controller): decoding and finger tracking.
// No ESP-IDF or LVGL here, so the fault handling can be tested on a PC (test/test_host.c).
// The report format follows Espressif's esp_lcd_touch_cst9220 driver.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CST92XX_REPORT_LEN 15        // read from 0xD000: up to 2 points x 5 bytes + 5 header bytes
#define CST92XX_OUTAGE_US 300000     // a press is cancelled after this long without a usable sample

typedef enum {
    CST_SAMPLE_FAILED,   // read or acknowledgement failed, or the report is malformed: says nothing
    CST_SAMPLE_NO_NEWS,  // the controller answered but has no new report: the finger did not change
    CST_SAMPLE_UP,       // new report: no finger
    CST_SAMPLE_DOWN,     // new report: finger at x, y (controller coordinates)
} cst_sample_kind_t;

typedef struct {
    cst_sample_kind_t kind;
    uint16_t x, y;
} cst_sample_t;

// r: the report. read_ok: the I2C read worked. ack_ok: the acknowledgement was written, or the firmware needs
// none. A report that could not be acknowledged is FAILED: the controller keeps it and returns it again.
cst_sample_t cst92xx_decode(const uint8_t *r, bool read_ok, bool ack_ok);

typedef enum {
    CST_OUT_UP,      // no finger
    CST_OUT_DOWN,    // finger at the tracker's x, y
    CST_OUT_CANCEL,  // end the press without it counting as a tap (once; then UP until the finger is lifted)
} cst_out_t;

typedef struct {
    bool down;            // finger reported to the UI
    bool suppressed;      // press was cancelled: ignore this finger until the controller reports none
    uint16_t x, y;        // last position
    int64_t last_ok_us;   // time of the last sample that was not FAILED
} cst_tracker_t;

// Feed one sample (zero-initialise the tracker first). now_us must not go backwards.
cst_out_t cst92xx_track(cst_tracker_t *t, cst_sample_t s, int64_t now_us);
