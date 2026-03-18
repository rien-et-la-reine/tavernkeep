add_library( bartap
    bartap.c
    )

target_link_libraries(bartap
    ff16
)

target_include_directories(bartap PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")