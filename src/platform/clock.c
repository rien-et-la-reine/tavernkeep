//handles get_fattime(), timestamp calculation
//
//see clock.h for why the clock is not valid on the development hardware. this file owns the one seam
//FatFs has for time: get_fattime() (third_party/fatfs/documents/doc/fattime.html). it is compiled
//because ffconf.h sets FF_FS_NORTC to 0; with the default of 1 FatFs would generate a fixed timestamp
//itself and the seam would not exist.

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

#include "platform/clock.h"

//FAT timestamp packing, per fattime.html:
//  bits 31..25  year - 1980      (0..127)
//  bits 24..21  month            (1..12)
//  bits 20..16  day              (1..31)
//  bits 15..11  hour             (0..23)
//  bits 10..5   minute           (0..59)
//  bits  4..0   second / 2       (0..29)
//
//the sentinel is 1980-01-01 00:00:00, the earliest representable FAT time. hosts display it as the epoch
//or as "unknown", which is the honest statement while no valid clock exists
#define FAT_TIME_SENTINEL_1980 \
    (((DWORD)(1980 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16))

bool clock_is_valid(void)
{
    //TODO(first pcb): true once the AON timer has been set from a trusted source and is running from an
    //accurate tick source; persist the validity flag with the rest of device state
    return false;
}

DWORD get_fattime(void)
{
    if (!clock_is_valid()) {
        return FAT_TIME_SENTINEL_1980;
    }
    //TODO(first pcb): read the AON timer, convert to civil time, pack per the table above
    return FAT_TIME_SENTINEL_1980;
}
