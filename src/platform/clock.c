//wall-clock time
//
//see clock.h for why the clock is not valid on the development hardware. FatFs's time seam,
//get_fattime(), is not here: it lives in the filesystem layer (src/storage/filesystem.c), which packs the
//time this file reports into FAT's format, so that nothing outside that layer includes ff.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform/clock.h"

bool clock_is_valid(void)
{
    //TODO(first pcb): true once the AON timer has been set from a trusted source and is running from an
    //accurate tick source; persist the validity flag with the rest of device state
    return false;
}

bool clock_get_datetime(clock_datetime_t *now)
{
    if (now == NULL || !clock_is_valid()) {
        return false;
    }
    //TODO(first pcb): read the AON timer and convert to civil time
    return false;
}
