add_library( ff16
    ff.h
    ff.c
    ffsystem.c
    ffconf.h
    ffunicode.c
    diskio.h
    )

target_include_directories(ff16 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")