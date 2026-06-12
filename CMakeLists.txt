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

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(X264 REQUIRED x264)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(V4L2 REQUIRED libv4l2)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(SRT REQUIRED srt)

set(ELEMENT_INCLUDE_DIRS
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${X264_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${V4L2_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${SRT_INCLUDE_DIRS}
)

set(ELEMENT_LIBRARIES
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${X264_LIBRARIES}
    ${ALSA_LIBRARIES}
    ${V4L2_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${SRT_LIBRARIES}
)

# ── Include directories ──────────────────────────────────────────────────
set(ZSTREAMER_INCLUDE_DIR
    ${CMAKE_SOURCE_DIR}/include
)

# ── Core library sources ─────────────────────────────────────────────────
set(CORE_SOURCES
    src/zst_caps.c
    src/zst_buffer.c
    src/zst_buffer_pool.c
    src/zst_pad.c
    src/zst_element.c
    src/zst_bin.c
    src/zst_pipeline.c
    src/zst_queue.c
    src/zst_queue_element.c
    src/zst_scheduler.c
    src/zst_bus.c
    src/zst_log.c
    src/zst_allocator.c
    src/zst_clock.c
)

if(ENABLE_PLUGINS)
    list(APPEND CORE_SOURCES src/zst_plugin.c)
endif()

# ── Build the core library ───────────────────────────────────────────────
if(BUILD_SHARED)
    add_library(zstreamer SHARED ${CORE_SOURCES})
else()
    add_library(zstreamer STATIC ${CORE_SOURCES})
endif()

include(GNUInstallDirs)

target_include_directories(zstreamer
    PUBLIC
        $<BUILD_INTERFACE:${ZSTREAMER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include/zstreamer>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Required for shared plugin libraries that link against the static lib
set_target_properties(zstreamer PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(zstreamer
    PUBLIC  Threads::Threads
    PRIVATE ${DL_LIB}
)

target_compile_definitions(zstreamer PRIVATE
    ZSTREAMER_DEFAULT_PLUGIN_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/zstreamer/plugins"
)

# ── Build the elements library ───────────────────────────────────────────
set(ELEMENT_LIBRARY_SOURCES
    src/zst_builtins.c
    src/v4l2_source.c
    src/alsa_source.c
    src/h264_encoder.c
    src/h264_decoder.c
    src/h265_encoder.c
    src/h265_decoder.c
    src/aac_encoder.c
    src/aac_decoder.c
    src/mp4_muxer.c
    src/file_sink.c
    src/file_source.c
    src/fake_sink.c
    src/video_scaler.c
    src/audio_resampler.c
    src/video_test_src.c
    src/audio_test_src.c
    src/text_overlay.c
    src/text_source.c
    src/srt_parser.c
    src/net_source.c
    src/net_sink.c
    src/rtsp_source.c
    src/rtsp_sink.c
    src/rtsp_server.c
    src/rtmp_source.c
    src/rtmp_sink.c
    src/srt_source.c
    src/srt_sink.c
    src/mpegts_muxer.c
    src/mpegts_demuxer.c
    src/mp4_demuxer.c
    src/http_source.c
)

if(BUILD_SHARED)
    add_library(zstreamer-elements SHARED ${ELEMENT_LIBRARY_SOURCES})
else()
    add_library(zstreamer-elements STATIC ${ELEMENT_LIBRARY_SOURCES})
endif()

target_include_directories(zstreamer-elements
    PUBLIC
        $<BUILD_INTERFACE:${ZSTREAMER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include/zstreamer>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${ELEMENT_INCLUDE_DIRS}
)

set_target_properties(zstreamer-elements PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(zstreamer-elements
    PUBLIC  zstreamer
    PRIVATE ${ELEMENT_LIBRARIES}
)

# ── Element plugins (built-in) ──────────────────────────────────────────
# These live inside the same library for now.  In the future they can be
# compiled as separate .so files and loaded via zst_plugin.
set(ELEMENT_SOURCES
    src/v4l2_source.c
    src/h264_encoder.c
    src/mp4_muxer.c
    src/file_sink.c
    src/fake_sink.c
    src/alsa_source.c
    src/aac_encoder.c
    src/video_scaler.c
    src/audio_resampler.c
    src/text_overlay.c
    src/video_test_src.c
    src/audio_test_src.c
    src/file_source.c
    src/text_source.c
    src/h264_decoder.c
    src/h265_encoder.c
    src/h265_decoder.c
    src/aac_decoder.c
    src/net_source.c
    src/net_sink.c
    src/rtmp_source.c
    src/rtmp_sink.c
    src/http_source.c
)

# ── Dynamic Plugins ──────────────────────────────────────────────────────
add_library(zst_filesink SHARED src/file_sink.c)
target_compile_definitions(zst_filesink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_filesink PRIVATE zstreamer)

add_library(zst_fakesink SHARED src/fake_sink.c)
target_compile_definitions(zst_fakesink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_fakesink PRIVATE zstreamer)

add_library(zst_v4l2source SHARED src/v4l2_source.c)
target_compile_definitions(zst_v4l2source PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_v4l2source PRIVATE zstreamer ${V4L2_LIBRARIES})

add_library(zst_rtspsource SHARED src/rtsp_source.c)
target_compile_definitions(zst_rtspsource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtspsource PRIVATE zstreamer ${AVUTIL_LIBRARIES})

add_library(zst_rtspsink SHARED src/rtsp_sink.c)
target_compile_definitions(zst_rtspsink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtspsink PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_rtsp_server SHARED src/rtsp_server.c)
target_compile_definitions(zst_rtsp_server PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtsp_server PRIVATE zstreamer)

add_library(zst_alsasource SHARED src/alsa_source.c)
target_compile_definitions(zst_alsasource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_alsasource PRIVATE zstreamer ${ALSA_LIBRARIES})

add_library(zst_h264encoder SHARED src/h264_encoder.c)
target_compile_definitions(zst_h264encoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_h264encoder PRIVATE zstreamer ${X264_LIBRARIES})

add_library(zst_aacencoder SHARED src/aac_encoder.c)
target_compile_definitions(zst_aacencoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_aacencoder PRIVATE zstreamer ${AVCODEC_LIBRARIES})

add_library(zst_mp4muxer SHARED src/mp4_muxer.c)
target_compile_definitions(zst_mp4muxer PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_mp4muxer PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_videoscaler SHARED src/video_scaler.c)
target_compile_definitions(zst_videoscaler PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_videoscaler PRIVATE zstreamer ${SWSCALE_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_audioresampler SHARED src/audio_resampler.c)
target_compile_definitions(zst_audioresampler PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_audioresampler PRIVATE zstreamer ${SWRESAMPLE_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_textoverlay SHARED src/text_overlay.c)
target_compile_definitions(zst_textoverlay PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_textoverlay PRIVATE zstreamer ${FREETYPE_LIBRARIES})

add_library(zst_srtparser SHARED src/srt_parser.c)
target_compile_definitions(zst_srtparser PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_srtparser PRIVATE zstreamer)

add_library(zst_videotestsrc SHARED src/video_test_src.c)
target_compile_definitions(zst_videotestsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_videotestsrc PRIVATE zstreamer)

add_library(zst_audiotestsrc SHARED src/audio_test_src.c)
target_compile_definitions(zst_audiotestsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_audiotestsrc PRIVATE zstreamer)

add_library(zst_filesource SHARED src/file_source.c)
target_compile_definitions(zst_filesource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_filesource PRIVATE zstreamer)

add_library(zst_textsource SHARED src/text_source.c)
target_compile_definitions(zst_textsource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_textsource PRIVATE zstreamer ${FREETYPE_LIBRARIES})

add_library(zst_netsrc SHARED src/net_source.c)
target_compile_definitions(zst_netsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_netsrc PRIVATE zstreamer)

add_library(zst_netsink SHARED src/net_sink.c)
target_compile_definitions(zst_netsink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_netsink PRIVATE zstreamer)

add_library(zst_h264decoder SHARED src/h264_decoder.c)
target_compile_definitions(zst_h264decoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_h264decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_h265encoder SHARED src/h265_encoder.c)
target_compile_definitions(zst_h265encoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_h265encoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_h265decoder SHARED src/h265_decoder.c)
target_compile_definitions(zst_h265decoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_h265decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_aacdecoder SHARED src/aac_decoder.c)
target_compile_definitions(zst_aacdecoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_aacdecoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_rtmpsource SHARED src/rtmp_source.c)
target_compile_definitions(zst_rtmpsource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtmpsource PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_rtmpsink SHARED src/rtmp_sink.c)
target_compile_definitions(zst_rtmpsink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtmpsink PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_srtsource SHARED src/srt_source.c)
target_compile_definitions(zst_srtsource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_srtsource PRIVATE zstreamer ${SRT_LIBRARIES})

add_library(zst_srtsink SHARED src/srt_sink.c)
target_compile_definitions(zst_srtsink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_srtsink PRIVATE zstreamer ${SRT_LIBRARIES})

add_library(zst_mpegtsmux SHARED src/mpegts_muxer.c)
target_compile_definitions(zst_mpegtsmux PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_mpegtsmux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_mpegtsdemux SHARED src/mpegts_demuxer.c)
target_compile_definitions(zst_mpegtsdemux PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_mpegtsdemux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_mp4demux SHARED src/mp4_demuxer.c)
target_compile_definitions(zst_mp4demux PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_mp4demux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

add_library(zst_httpsource SHARED src/http_source.c)
target_compile_definitions(zst_httpsource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_httpsource PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})

set(PLUGIN_TARGETS zst_filesink zst_fakesink zst_v4l2source zst_rtspsource zst_rtspsink zst_rtsp_server zst_alsasource zst_h264encoder zst_aacencoder zst_mp4muxer zst_videoscaler zst_audioresampler zst_textoverlay zst_srtparser zst_videotestsrc zst_audiotestsrc zst_filesource zst_textsource zst_netsrc zst_netsink zst_h264decoder zst_h265encoder zst_h265decoder zst_aacdecoder zst_rtmpsource zst_rtmpsink zst_srtsource zst_srtsink zst_mpegtsmux zst_mpegtsdemux zst_mp4demux zst_httpsource)
foreach(plugin ${PLUGIN_TARGETS})
    target_include_directories(${plugin} PRIVATE ${ZSTREAMER_INCLUDE_DIR} PRIVATE ${ELEMENT_INCLUDE_DIRS})
    set_target_properties(${plugin} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
endforeach()


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
        PRIVATE zstreamer-elements
    )

    add_executable(test_net_source
        tests/test_net_source.c
    )
    target_include_directories(test_net_source
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_net_source
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_net_source COMMAND test_net_source)

    add_executable(test_net_sink
        tests/test_net_sink.c
    )
    target_include_directories(test_net_sink
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_net_sink
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_net_sink COMMAND test_net_sink)

    add_executable(test_http_source
        tests/test_http_source.c
    )
    target_include_directories(test_http_source
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_http_source
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_http_source COMMAND test_http_source)


    # -- full pipeline example --
    add_executable(example_record
        tests/example_record.c
    )
    target_include_directories(example_record
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(example_record
        PRIVATE zstreamer-elements
    )

    # -- deterministic 10s colorbar/timecode H.264 + 1kHz AAC MP4 demo --
    # Uses the Element Public API (typed property setters) and
    # zst_register_builtin_elements() for factory-based element creation.
    # zst_register_builtin_elements() lives in libzstreamer-elements and uses
    # strong (non-weak) constructor references, so no --whole-archive needed.
    add_executable(demo_colorbar_mp4
        tests/demo_colorbar_mp4.c
    )
    target_include_directories(demo_colorbar_mp4
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(demo_colorbar_mp4
        PRIVATE zstreamer-elements
    )

    add_executable(example_text_overlay
        tests/example_text_overlay.c
    )
    target_include_directories(example_text_overlay
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(example_text_overlay
        PRIVATE zstreamer-elements
    )

    # Register with CTest
    include(CTest)
    enable_testing()

    add_test(NAME test_core
        COMMAND test_core
    )

    add_test(NAME test_install
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install.sh ${CMAKE_CURRENT_BINARY_DIR}
    )
endif()

# ── Compiler warnings ────────────────────────────────────────────────────
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

# ── Installation ─────────────────────────────────────────────────────────
# Install library targets with EXPORT support
install(TARGETS zstreamer zstreamer-elements
    EXPORT zstreamerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install dynamic plugins
install(TARGETS ${PLUGIN_TARGETS}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zstreamer/plugins
)

# Install public headers
file(GLOB ROOT_HEADERS "include/*.h")
install(FILES ${ROOT_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zstreamer
)
install(DIRECTORY include/zstreamer/elements
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zstreamer
)

# ── pkg-config ───────────────────────────────────────────────────────────
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zstreamer.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer.pc"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zstreamer-elements.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer-elements.pc"
    @ONLY
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer.pc"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer-elements.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# ── CMake Export / config ────────────────────────────────────────────────
install(EXPORT zstreamerTargets
    FILE zstreamerTargets.cmake
    NAMESPACE zstreamer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zstreamer
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zstreamerConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zstreamer
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zstreamer
)
