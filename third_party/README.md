# Third-party code

External source dependencies belong here and should retain their upstream
licenses. FatFs is vendored at `third_party/fatfs/` with its upstream license
and documentation; it is not yet built by either CMake tree or adapted to the
block-device interface.

FatFs config options changed from default:
- FF_CODE_PAGE: 437 (US, smaller than JIS, only comes up for files without long names, long names use unicode regardless)
- FF_USE_LFN: 2 (EPUB uses LFN)
- FF_LFN_UNICODE: 2 (UTF-8)
- FF_USE_MKFS: 1 (in case of SDXC exFAT formatted card, can reformat to FAT32 on device)
- FF_USE_LABEL: 1 (UI can show card volume label)
- FF_USE_FIND: 1 (for navigation)
- FF_NORTC_YEAR: 1980 (not strictly necessary since FF_FS_NORTC is 0 and application will supply get_fattime())
Deliberately left at default:
- FF_FS_EXFAT: left at 0 for licensing reasons, no exFAT support planned currently or in future