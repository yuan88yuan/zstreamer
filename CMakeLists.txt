cmake_minimum_required(VERSION 3.16)
project(zstreamer VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Options ──────────────────────────────────────────────────────────────
option(BUILD_TESTS     "Build test programs" ON)
option(BUILD_SHARED    "Build core as shared library" OFF)
option(ENABLE_PLUGINS  "Enable dlopen-based plugin loading" ON)

# ── Dependencies ─────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

if(ENABLE_PLUGINS)
    # dlopen / libdl
    if(UNIX)
        set(DL_LIB ${CMAKE_DL_LIBS})
    endif()
endif()

# ── Include directories ──────────────────────────────────────────────────
set(ZSTREAMER_INCLUDE_DIR
    ${CMAKE_SOURCE_DIR}/include
)

# ── Core library sources ─────────────────────────────────────────────────
set(CORE_SOURCES
    src/mm_buffer.c
    src/mm_pad.c
    src/mm_element.c
    src/mm_pipeline.c
    src/mm_queue.c
    src/mm_scheduler.c
)

if(ENABLE_PLUGINS)
    list(APPEND CORE_SOURCES src/mm_plugin.c)
endif()

# ── Build the core library ───────────────────────────────────────────────
if(BUILD_SHARED)
    add_library(zstreamer SHARED ${CORE_SOURCES})
else()
    add_library(zstreamer STATIC ${CORE_SOURCES})
endif()

target_include_directories(zstreamer
    PUBLIC  ${ZSTREAMER_INCLUDE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(zstreamer
    PUBLIC  Threads::Threads
    PRIVATE ${DL_LIB}
)

# Export include directory for find_package / fetchcontent consumers
target_include_directories(zstreamer
    INTERFACE $<INSTALL_INTERFACE:include>
)

# ── Element plugins (built-in) ──────────────────────────────────────────
# These live inside the same library for now.  In the future they can be
# compiled as separate .so files and loaded via mm_plugin.
set(ELEMENT_SOURCES
    src/v4l2_source.c
    src/h264_encoder.c
    src/mp4_muxer.c
    src/file_sink.c
)

# ── Tests ────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    # -- unit / smoke test for core infrastructure --
    add_executable(test_core
        tests/test_core.c
    )
    target_include_directories(test_core
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_core
        PRIVATE zstreamer
    )

    # -- full pipeline example --
    add_executable(example_record
        tests/example_record.c
        ${ELEMENT_SOURCES}
    )
    target_include_directories(example_record
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(example_record
        PRIVATE zstreamer
    )

    # Register with CTest
    include(CTest)
    enable_testing()

    add_test(NAME test_core
        COMMAND test_core
    )
endif()

# ── Installation ─────────────────────────────────────────────────────────
install(TARGETS zstreamer
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ── Compiler warnings ────────────────────────────────────────────────────
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()
