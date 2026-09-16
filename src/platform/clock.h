#ifndef TAVERNKEEP_PLATFORM_CLOCK_H
#define TAVERNKEEP_PLATFORM_CLOCK_H

//wall-clock time for the firmware
//
//the RP2350 has no RTC peripheral; its always-on (AON/POWMAN) timer can serve as one once the first
//board provides an accurate low-frequency tick source and a way to set the time. until then the clock
//is never valid, and every consumer must treat "not valid" as "unknown" rather than as a time. FatFs's
//get_fattime() (declared in ff.h, implemented in clock.c) is the first consumer: it returns a fixed
//sentinel timestamp while the clock is not valid, so files carry an obviously-absent date instead of a
//plausible wrong one.

#include <stdbool.h>

//true once a trusted time has been set and is being kept; false on this development hardware
bool clock_is_valid(void);

#endif
