# Third-party code

External source dependencies belong here and should retain their upstream
licenses.

## FatFs

FatFs R0.16 is vendored at `third_party/fatfs/` with its upstream license and
documentation. It is the implementation of the filesystem layer
(`src/storage/filesystem.c`) and is private to it: nothing else in the firmware
calls FatFs or includes its headers (`docs/architecture.md`, "Filesystem").

How it is built (`CMakeLists.txt`):
- `ff.c` and `ffunicode.c` are compiled into the firmware.
- `diskio.c` here is upstream's stub and is deliberately not compiled;
  `src/storage/diskio.c` implements the disk I/O functions over the
  block-device interface instead.
- `ffsystem.c` is not compiled: it is only needed for `FF_USE_LFN` 3 (heap) or
  `FF_FS_REENTRANT` 1. Add it if either changes.
- The FatFs include path is given only to `src/storage/filesystem.c` and
  `src/storage/diskio.c`, so an `#include "ff.h"` anywhere else fails to build.
- `get_fattime()`, FatFs's time source, is supplied by
  `src/storage/filesystem.c` from `platform/clock.h`.

Host tests for the layer, including FatFs, live in `tests/filesystem/` (kept
locally, not in the repository).

FatFs config options changed from default (`source/ffconf.h`):
- FF_CODE_PAGE: 437 (US, smaller than JIS, only comes up for files without long names, long names use unicode regardless)
- FF_USE_LFN: 2 (EPUB uses LFN). The long-file-name working buffer goes on the caller's stack, which is why the firmware stack is 4 KB (`CMakeLists.txt`)
- FF_LFN_UNICODE: 2 (UTF-8)
- FF_USE_MKFS: 1 (an SDXC card arrives formatted as exFAT, so it can be reformatted to FAT on the device; `filesystem_format`)
- FF_USE_LABEL: 1 (UI can show card volume label)
- FF_USE_FIND: 1 (pattern-filtered directory listing; `filesystem_opendir`)
- FF_NORTC_YEAR: 1980 (not strictly necessary since FF_FS_NORTC is 0 and the firmware supplies get_fattime())

Deliberately left at default:
- FF_FS_EXFAT: 0 for licensing reasons, no exFAT support planned currently or in future. Sizes and offsets in the filesystem interface are therefore 32-bit
- FF_FS_LOCK: 0. FatFs does not refuse removing, renaming or re-opening an open file; not doing so is a documented caller rule instead (`src/storage/filesystem.h`)
- FF_FS_REENTRANT: 0. The filesystem layer is single-threaded and called from one core only
- FF_FS_TINY: 0. Each open file has its own sector buffer: faster, at about 512 bytes of RAM per file slot
- FF_VOLUMES: 1, FF_MAX_SS: 512, FF_LBA64: 0. One card, 512-byte sectors, 32-bit sector numbers; the disk I/O adapter refuses a device that does not fit these
