#include "rtc.h"
#include "io.h"

/* CMOS RTC ports */
#define RTC_ADDR 0x70
#define RTC_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(RTC_ADDR, reg);
    return inb(RTC_DATA);
}

static int update_in_progress(void) {
    outb(RTC_ADDR, 0x0A);
    return inb(RTC_DATA) & 0x80;
}

static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

void rtc_read(rtc_time_t *t) {
    while (update_in_progress()) {}
    uint8_t ss = cmos_read(0x00);
    uint8_t mm = cmos_read(0x02);
    uint8_t hh = cmos_read(0x04);
    uint8_t dy = cmos_read(0x07);
    uint8_t mo = cmos_read(0x08);
    uint8_t yr = cmos_read(0x09);
    uint8_t st = cmos_read(0x0B);

    /* 12-hour mode: strip the PM bit BEFORE the BCD conversion, or
     * bcd2bin(0x87) turns "7 PM" into hour 87 and loses the PM flag */
    uint8_t pm = 0;
    if (!(st & 0x02)) { pm = hh & 0x80; hh &= 0x7F; }

    /* If bit 2 of register B is 0, values are BCD */
    if (!(st & 0x04)) {
        ss = bcd2bin(ss); mm = bcd2bin(mm); hh = bcd2bin(hh);
        dy = bcd2bin(dy); mo = bcd2bin(mo); yr = bcd2bin(yr);
    }
    /* 24-hour conversion in the binary domain */
    if (pm)        { if (hh != 12) hh += 12; }
    else if (hh == 12) hh = 0;
    t->second = ss; t->minute = mm; t->hour = hh;
    t->day = dy; t->month = mo; t->year = 2000 + yr;
}
