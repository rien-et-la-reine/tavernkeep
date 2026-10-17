#ifndef TAVERNKEEP_PLATFORM_CLOCK_H
#define TAVERNKEEP_PLATFORM_CLOCK_H

//wall-clock time for the firmware
//
//the RP2350 has no RTC peripheral; its always-on (AON/POWMAN) timer can serve as one once the first
//board provides an accurate low-frequency tick source and a way to set the time. until then the clock
//is never valid, and every consumer must treat "not valid" as "unknown" rather than as a time. FatFs's
//get_fattime() (implemented in src/storage/filesystem.c, so that ff.h stays inside the filesystem
//layer) is the first consumer: it stamps files with a fixed sentinel while the clock is not valid, so
//they carry an obviously-absent date instead of a plausible wrong one.

#include <stdbool.h>
#include <stdint.h>

//civil time, local to the device
typedef struct {
    uint16_t year;      //e.g. 2026
    uint8_t month;      //1..12
    uint8_t day;        //1..31
    uint8_t hour;       //0..23
    uint8_t minute;     //0..59
    uint8_t second;     //0..59
} clock_datetime_t;

//true once a trusted time has been set and is being kept; false on this development hardware
bool clock_is_valid(void);

//fill *now with the current time and return true, or return false with *now unspecified while the
//clock is not valid
bool clock_get_datetime(clock_datetime_t *now);

#endif
