cmake_minimum_required(VERSION 3.16)
project(zstreamer VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(_GNU_SOURCE)


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
find_package(Vulkan REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(X264 REQUIRED x264)
pkg_check_modules(X265 REQUIRED x265)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(V4L2 REQUIRED libv4l2)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(FREETYPE REQUIRED freetype2)

# SRT packaging is distro-dependent.  FFmpeg's libavformat may be linked
# against libsrt-gnutls.so while a plain `pkg-config srt` can resolve to a
# different SRT shared object on other systems.  Loading two SRT variants into
# one process can crash, so choose the zstreamer SRT library by matching
# libavformat's actual DT_NEEDED entry instead of assuming from FFmpeg version.
pkg_check_modules(SRT_PC srt)
if(NOT SRT_PC_FOUND)
    pkg_check_modules(SRT_PC REQUIRED srt-gnutls)
endif()
set(SRT_INCLUDE_DIRS ${SRT_PC_INCLUDE_DIRS})
set(SRT_LIBRARY_DIRS ${SRT_PC_LIBRARY_DIRS})

find_library(SRT_GNUTLS_LIBRARY NAMES srt-gnutls HINTS ${SRT_PC_LIBRARY_DIRS})
find_library(SRT_GENERIC_LIBRARY NAMES srt HINTS ${SRT_PC_LIBRARY_DIRS})
find_library(AVFORMAT_LIBRARY NAMES avformat HINTS ${AVFORMAT_LIBRARY_DIRS})

set(ZST_FFMPEG_SRT_BACKEND "none")
set(ZST_AVFORMAT_DYNAMIC_SECTION "")
if(UNIX AND NOT CMAKE_CROSSCOMPILING AND AVFORMAT_LIBRARY)
    find_program(READELF_EXECUTABLE NAMES readelf)
    if(READELF_EXECUTABLE)
        execute_process(
            COMMAND ${READELF_EXECUTABLE} -d ${AVFORMAT_LIBRARY}
            RESULT_VARIABLE ZST_READELF_RESULT
            OUTPUT_VARIABLE ZST_AVFORMAT_DYNAMIC_SECTION
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    elseif(CMAKE_OBJDUMP)
        execute_process(
            COMMAND ${CMAKE_OBJDUMP} -p ${AVFORMAT_LIBRARY}
            RESULT_VARIABLE ZST_READELF_RESULT
            OUTPUT_VARIABLE ZST_AVFORMAT_DYNAMIC_SECTION
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()

    if(ZST_AVFORMAT_DYNAMIC_SECTION MATCHES "libsrt-gnutls\\.so")
        set(ZST_FFMPEG_SRT_BACKEND "gnutls")
    elseif(ZST_AVFORMAT_DYNAMIC_SECTION MATCHES "libsrt[^/\n]*\\.so")
        set(ZST_FFMPEG_SRT_BACKEND "generic")
    endif()
endif()

if(ZST_FFMPEG_SRT_BACKEND STREQUAL "gnutls")
    if(NOT SRT_GNUTLS_LIBRARY)
        message(FATAL_ERROR
            "libavformat is linked against libsrt-gnutls, but libsrt-gnutls "
            "was not found for zstreamer. Install the matching SRT development "
            "package or disable one of the conflicting components."
        )
    endif()
    set(SRT_LIBRARIES ${SRT_GNUTLS_LIBRARY})
    set(ZST_SRT_LINK_BACKEND "libsrt-gnutls (matched libavformat)")
elseif(ZST_FFMPEG_SRT_BACKEND STREQUAL "generic")
    if(NOT SRT_GENERIC_LIBRARY)
        message(FATAL_ERROR
            "libavformat is linked against libsrt.so, but libsrt.so was not "
            "found for zstreamer. Install the matching SRT development package "
            "or disable one of the conflicting components."
        )
    endif()
    set(SRT_LIBRARIES ${SRT_GENERIC_LIBRARY})
    set(ZST_SRT_LINK_BACKEND "libsrt (matched libavformat)")
else()
    # No SRT dependency was detected in libavformat, or this is a cross/static
    # build where DT_NEEDED cannot be inspected.  Prefer the GnuTLS SRT variant
    # when present because that is the common FFmpeg packaging choice on Ubuntu,
    # then fall back to libsrt.so, then finally to pkg-config's library list.
    if(SRT_GNUTLS_LIBRARY)
        set(SRT_LIBRARIES ${SRT_GNUTLS_LIBRARY})
        set(ZST_SRT_LINK_BACKEND "libsrt-gnutls (preferred; libavformat SRT not detected)")
    elseif(SRT_GENERIC_LIBRARY)
        set(SRT_LIBRARIES ${SRT_GENERIC_LIBRARY})
        set(ZST_SRT_LINK_BACKEND "libsrt (fallback; libavformat SRT not detected)")
    else()
        set(SRT_LIBRARIES ${SRT_PC_LIBRARIES})
        set(ZST_SRT_LINK_BACKEND "${SRT_PC_LIBRARIES} (pkg-config fallback)")
    endif()
endif()

message(STATUS "libavformat SRT backend: ${ZST_FFMPEG_SRT_BACKEND}")
message(STATUS "zstreamer SRT link: ${ZST_SRT_LINK_BACKEND}")

find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    add_compile_definitions(HAS_CUDA=1)
endif()

# ── NVIDIA Jetson Allocator Support ──────────────────────────────────
option(ENABLE_JETSON "Enable Jetson NvBuffer allocator support" OFF)
if(ENABLE_JETSON)
    add_compile_definitions(HAS_JETSON=1)
    find_library(NVBUF_UTILS_LIBRARY NAMES nvbuf_utils)
    if(NVBUF_UTILS_LIBRARY)
        set(JETSON_LIBRARIES ${NVBUF_UTILS_LIBRARY})
        message(STATUS "Jetson NvBuffer utils found")
    else()
        message(WARNING "Jetson NvBuffer utils not found, falling back to stub for tests")
        set(JETSON_LIBRARIES "")
        # Add stub path
        include_directories(tests)
        set(STUB_SOURCES tests/stub_nvbuf_utils.cpp)
    endif()
endif()

# ── Intel oneAPI SYCL Allocator Support ──────────────────────────────
option(ENABLE_ONEAPI "Enable Intel oneAPI SYCL allocator support" ON)
if(ENABLE_ONEAPI)
    enable_language(CXX OPTIONAL)
    if(CMAKE_CXX_COMPILER_LOADED)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS "-fsycl")
        check_cxx_source_compiles("
            #include <sycl/sycl.hpp>
            int main() {
                sycl::queue q;
                return 0;
            }
        " HAS_SYCL_SUPPORT)
        unset(CMAKE_REQUIRED_FLAGS)

        if(HAS_SYCL_SUPPORT)
            set(HAS_ONEAPI 1)
            add_compile_definitions(HAS_ONEAPI=1)
            message(STATUS "Intel oneAPI SYCL allocator support enabled (-fsycl)")
        else()
            find_package(sycl QUIET)
            if(sycl_FOUND)
                set(HAS_ONEAPI 1)
                add_compile_definitions(HAS_ONEAPI=1)
                message(STATUS "SYCL allocator support enabled via find_package(sycl)")
            else()
                message(STATUS "Intel oneAPI SYCL allocator support not found or compiler lacks -fsycl")
            endif()
        endif()
    else()
        message(STATUS "C++ compiler not loaded, skipping Intel oneAPI SYCL check")
    endif()
endif()

set(ELEMENT_INCLUDE_DIRS
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${X264_INCLUDE_DIRS}
    ${X265_INCLUDE_DIRS}
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
    ${X265_LIBRARIES}
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

if(HAS_ONEAPI)
    list(APPEND CORE_SOURCES src/zst_allocator_oneapi.cpp)
endif()

if(ENABLE_JETSON)
    list(APPEND CORE_SOURCES src/zst_allocator_jetson.cpp ${STUB_SOURCES})
endif()

# ── Build the core library ───────────────────────────────────────────────
if(BUILD_SHARED)
    add_library(zstreamer SHARED ${CORE_SOURCES})
else()
    add_library(zstreamer STATIC ${CORE_SOURCES})
endif()

if(HAS_ONEAPI)
    if(HAS_SYCL_SUPPORT)
        set_source_files_properties(src/zst_allocator_oneapi.cpp PROPERTIES COMPILE_OPTIONS "-fsycl")
        target_link_options(zstreamer PUBLIC "-fsycl")
        set_target_properties(zstreamer PROPERTIES LINKER_LANGUAGE CXX)
    endif()
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
    PRIVATE ${DL_LIB} Vulkan::Vulkan
)

if(ENABLE_JETSON)
    target_link_libraries(zstreamer PRIVATE ${JETSON_LIBRARIES})
endif()

if(CUDAToolkit_FOUND)
    target_link_libraries(zstreamer PUBLIC CUDA::cudart)
    set(ZSTREAMER_PKG_LIBS "-lcudart")
else()
    set(ZSTREAMER_PKG_LIBS "")
endif()

target_compile_definitions(zstreamer PRIVATE
    ZSTREAMER_DEFAULT_PLUGIN_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/zstreamer/plugins"
)

# ── Build the elements library ───────────────────────────────────────────
set(ELEMENT_LIBRARY_SOURCES
    src/zst_builtins.c
    src/v4l2_source.c
    src/v4l2_sink.c
    src/alsa_source.c
    src/alsa_sink.c
    src/x264_encoder.c
    src/x265_encoder.c
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
    src/nv_video_scaler.c
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
    src/srt_source.c src/srt_common.c
    src/srt_sink.c src/srt_common.c
    src/mpegts_muxer.c
    src/mpegts_demuxer.c
    src/mp4_demuxer.c
    src/http_source.c
)

if(ENABLE_JETSON)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/nv_video_encoder.c src/nv_video_decoder.c)
endif()

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
    src/v4l2_sink.c
    src/x264_encoder.c
    src/x265_encoder.c
    src/mp4_muxer.c
    src/file_sink.c
    src/fake_sink.c
    src/alsa_source.c
    src/aac_encoder.c
    src/video_scaler.c
    src/nv_video_scaler.c
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

add_library(zst_v4l2sink SHARED src/v4l2_sink.c)
target_compile_definitions(zst_v4l2sink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_v4l2sink PRIVATE zstreamer ${V4L2_LIBRARIES})

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

add_library(zst_alsasink SHARED src/alsa_sink.c)
target_compile_definitions(zst_alsasink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_alsasink PRIVATE zstreamer ${ALSA_LIBRARIES})

add_library(zst_x264encoder SHARED src/x264_encoder.c)
target_compile_definitions(zst_x264encoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_x264encoder PRIVATE zstreamer ${X264_LIBRARIES})

add_library(zst_x265encoder SHARED src/x265_encoder.c)
target_compile_definitions(zst_x265encoder PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_x265encoder PRIVATE zstreamer ${X265_LIBRARIES})

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

add_library(zst_srt SHARED src/srt_source.c src/srt_sink.c src/srt_common.c src/srt_plugin.c)
target_compile_definitions(zst_srt PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_srt PRIVATE zstreamer ${SRT_LIBRARIES})

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

if(ENABLE_JETSON)
    add_library(zst_nvenc SHARED src/nv_video_encoder.c)
    target_compile_definitions(zst_nvenc PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_nvenc PRIVATE zstreamer ${JETSON_LIBRARIES})

    add_library(zst_nvdec SHARED src/nv_video_decoder.c)
    target_compile_definitions(zst_nvdec PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_nvdec PRIVATE zstreamer ${JETSON_LIBRARIES})

    set(JETSON_PLUGINS zst_nvenc zst_nvdec)
else()
    set(JETSON_PLUGINS "")
endif()

set(PLUGIN_TARGETS zst_filesink zst_fakesink zst_v4l2source zst_v4l2sink zst_rtspsource zst_rtspsink zst_rtsp_server zst_alsasource zst_alsasink zst_x264encoder zst_x265encoder zst_aacencoder zst_mp4muxer zst_videoscaler zst_audioresampler zst_textoverlay zst_srtparser zst_videotestsrc zst_audiotestsrc zst_filesource zst_textsource zst_netsrc zst_netsink zst_h264decoder zst_h265encoder zst_h265decoder zst_aacdecoder zst_rtmpsource zst_rtmpsink zst_srt zst_mpegtsmux zst_mpegtsdemux zst_mp4demux zst_httpsource ${JETSON_PLUGINS})
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

    add_executable(test_videotestsrc_timing
        tests/test_videotestsrc_timing.c
    )
    target_include_directories(test_videotestsrc_timing
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_videotestsrc_timing
        PRIVATE zstreamer-elements m
    )
    add_test(NAME test_videotestsrc_timing
        COMMAND test_videotestsrc_timing
    )

    add_executable(test_audiotestsrc_timing
        tests/test_audiotestsrc_timing.c
    )
    target_include_directories(test_audiotestsrc_timing
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_audiotestsrc_timing
        PRIVATE zstreamer-elements m
    )
    add_test(NAME test_audiotestsrc_timing
        COMMAND test_audiotestsrc_timing
    )

    add_executable(test_mp4demux_timing
        tests/test_mp4demux_timing.c
    )
    target_include_directories(test_mp4demux_timing
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_mp4demux_timing
        PRIVATE zstreamer-elements m
    )
    add_test(NAME test_mp4demux_timing
        COMMAND test_mp4demux_timing
    )

    add_executable(test_custom_paced_video_source
        tests/test_custom_paced_video_source.c
    )
    target_include_directories(test_custom_paced_video_source
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_custom_paced_video_source
        PRIVATE zstreamer-elements m
    )
    add_test(NAME test_custom_paced_video_source
        COMMAND test_custom_paced_video_source
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

    add_executable(test_v4l2_loopback
        tests/test_v4l2_loopback.c
    )
    target_include_directories(test_v4l2_loopback
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_v4l2_loopback
        PRIVATE zstreamer-elements
    )

    # -- full pipeline example --
    add_test(NAME test_nv_video_scaler COMMAND test_nv_video_scaler)

    add_executable(test_nv_video_scaler
        tests/test_nv_video_scaler.c
    )
    target_include_directories(test_nv_video_scaler
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_nv_video_scaler
        PRIVATE zstreamer-elements
    )

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

    # -- RTSP Media-On-Demand demo --
    add_executable(demo_rtsp_mod
        tests/demo_rtsp_mod.c
    )
    target_include_directories(demo_rtsp_mod
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(demo_rtsp_mod
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

# Append srt_common.c to the srt element targets

# ── Documentation ────────────────────────────────────────────────────────
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()
