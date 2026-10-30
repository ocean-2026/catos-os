#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {
    int year, month, day, hour, minute, second;
} rtc_time_t;

void rtc_read(rtc_time_t *t);

#endif
