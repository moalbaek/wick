#include "cst92xx.h"

#define REPORT_ACK 0xAB

cst_sample_t cst92xx_decode(const uint8_t *r, bool read_ok, bool ack_ok)
{
    cst_sample_t s = {CST_SAMPLE_FAILED, 0, 0};
    if (!read_ok || !ack_ok) return s;
    bool empty = true;
    for (int i = 0; i < 7; i++) empty = empty && r[i] == 0;
    if (empty) {
        s.kind = CST_SAMPLE_UP;
        return s;
    }
    if (r[0] == REPORT_ACK) {  // only our last acknowledgement: nothing new since then
        s.kind = CST_SAMPLE_NO_NEWS;
        return s;
    }
    int n = r[5] & 0x7F;
    if (r[6] != REPORT_ACK || n > 2) return s;
    s.kind = CST_SAMPLE_UP;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = &r[i == 0 ? 0 : i * 5 + 2];
        if (((p[0] & 0x0F) >> 1) == 3) {  // 0x06/0x07 = touching; release records keep the old position
            s.kind = CST_SAMPLE_DOWN;
            s.x = (uint16_t)(p[1] << 4 | p[3] >> 4);
            s.y = (uint16_t)(p[2] << 4 | (p[3] & 0x0F));
            break;
        }
    }
    return s;
}

cst_out_t cst92xx_track(cst_tracker_t *t, cst_sample_t s, int64_t now_us)
{
    if (s.kind != CST_SAMPLE_FAILED) t->last_ok_us = now_us;
    switch (s.kind) {
    case CST_SAMPLE_UP:
        t->down = false;
        t->suppressed = false;  // the cancelled finger is gone: the next touch is a new one
        return CST_OUT_UP;
    case CST_SAMPLE_DOWN:
        if (t->suppressed) return CST_OUT_UP;  // still the cancelled finger: it must not start a new press
        t->down = true;
        t->x = s.x;
        t->y = s.y;
        return CST_OUT_DOWN;
    case CST_SAMPLE_NO_NEWS:
        break;
    case CST_SAMPLE_FAILED:
        // Keep a press through short outages; after a long one nobody knows where the finger is.
        if (t->down && now_us - t->last_ok_us > CST92XX_OUTAGE_US) {
            t->down = false;
            t->suppressed = true;
            return CST_OUT_CANCEL;
        }
        break;
    }
    return t->down ? CST_OUT_DOWN : CST_OUT_UP;
}
