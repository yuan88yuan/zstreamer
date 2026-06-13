/*=============================================================================
    demo_rtsp_mod.c — RTSP Media-On-Demand Demo Application

    This demo runs an RTSP server with a dynamic mount callback.
    When a client connects and requests a stream:
      - /colorbar: dynamically creates and starts a synthetic H.264/AAC source
      - /bunny: dynamically starts an HTTPS source pulling Big Buck Bunny
                 and demuxing H.264/AAC packets directly to the client.
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_bus.h"
#include "zst_rtsp_server.h"
#include "zstreamer/elements/zst_mp4_demuxer.h"

static volatile int g_running = 1;
static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static zst_result_t on_demand_mount(zst_element_t* server, const char* name, void* user_data) {
    zst_pipeline_t* pipe = (zst_pipeline_t*)user_data;
    printf("[Demo App] Dynamic mount request received for session: /%s\n", name);

    if (strcmp(name, "colorbar") == 0) {
        printf("[Demo App] Creating colorbar source pipeline...\n");

        // 1. Add session to server
        if (zst_rtsp_server_add_session(server, name) != ZST_OK) {
            fprintf(stderr, "[Demo App] Failed to add RTSP session for /%s\n", name);
            return ZST_ERROR;
        }

        // 2. Create elements
        zst_element_t* video_src = zst_element_factory_make("videotestsrc");
        zst_element_t* overlay   = zst_element_factory_make("textoverlay");
        zst_element_t* h264      = zst_element_factory_make("h264enc");
        zst_element_t* audio_src = zst_element_factory_make("audiotestsrc");
        zst_element_t* aac       = zst_element_factory_make("aacenc");

        if (!video_src || !overlay || !h264 || !audio_src || !aac) {
            fprintf(stderr, "[Demo App] Failed to create colorbar pipeline elements\n");
            return ZST_ERROR;
        }

        // Configure elements
        zst_element_set_property_int(video_src, "width", 320);
        zst_element_set_property_int(video_src, "height", 240);
        zst_element_set_property_int(video_src, "fps", 30);
        zst_element_set_property_string(video_src, "pattern", "bars");
        zst_element_set_property_bool(video_src, "use-clock", true);

        zst_element_set_property_bool(overlay, "timecode", true);
        zst_element_set_property_int(overlay, "font-size", 24);
        zst_element_set_property_int(overlay, "x", 10);
        zst_element_set_property_int(overlay, "y", 35);

        zst_element_set_property_int(audio_src, "sample-rate", 44100);
        zst_element_set_property_int(audio_src, "channels", 2);
        zst_element_set_property_string(audio_src, "sample-format", "S16LE");
        zst_element_set_property_string(audio_src, "wave", "sine");
        zst_element_set_property_int(audio_src, "frequency", 1000);
        zst_element_set_property_int(audio_src, "samples-per-buffer", 1024);
        zst_element_set_property_bool(audio_src, "use-clock", true);

        // 3. Add to pipeline
        zst_pipeline_add(pipe, video_src);
        zst_pipeline_add(pipe, overlay);
        zst_pipeline_add(pipe, h264);
        zst_pipeline_add(pipe, audio_src);
        zst_pipeline_add(pipe, aac);

        // 4. Link pads
        // Video: video_src -> overlay -> h264 -> server (colorbar_video)
        zst_pad_link(zst_element_get_pad(video_src, "src"), zst_element_get_pad(overlay, "sink"));
        zst_pad_link(zst_element_get_pad(overlay, "src"), zst_element_get_pad(h264, "sink"));
        
        char pad_name[128];
        snprintf(pad_name, sizeof(pad_name), "%s_video", name);
        zst_pad_link(zst_element_get_pad(h264, "src"), zst_element_get_pad(server, pad_name));

        // Audio: audio_src -> aac -> server (colorbar_audio)
        zst_pad_link(zst_element_get_pad(audio_src, "src"), zst_element_get_pad(aac, "sink"));
        snprintf(pad_name, sizeof(pad_name), "%s_audio", name);
        zst_pad_link(zst_element_get_pad(aac, "src"), zst_element_get_pad(server, pad_name));

        // 5. Update pipeline execution sorting
        zst_pipeline_topological_sort(pipe);

        // 6. Set states
        zst_element_set_state(video_src, ZST_STATE_READY);
        zst_element_set_state(overlay, ZST_STATE_READY);
        zst_element_set_state(h264, ZST_STATE_READY);
        zst_element_set_state(audio_src, ZST_STATE_READY);
        zst_element_set_state(aac, ZST_STATE_READY);

        zst_element_set_state(video_src, ZST_STATE_PLAYING);
        zst_element_set_state(overlay, ZST_STATE_PLAYING);
        zst_element_set_state(h264, ZST_STATE_PLAYING);
        zst_element_set_state(audio_src, ZST_STATE_PLAYING);
        zst_element_set_state(aac, ZST_STATE_PLAYING);

        printf("[Demo App] Successfully mounted /%s source pipeline\n", name);
        return ZST_OK;
    }
    else if (strcmp(name, "bunny") == 0) {
        printf("[Demo App] Creating HTTP video source pipeline...\n");

        // 1. Add session to server
        if (zst_rtsp_server_add_session(server, name) != ZST_OK) {
            fprintf(stderr, "[Demo App] Failed to add RTSP session for /%s\n", name);
            return ZST_ERROR;
        }

        // 2. Create elements
        zst_element_t* demux = zst_element_factory_make("mp4demux");

        if (!demux) {
            fprintf(stderr, "[Demo App] Failed to create demux element\n");
            return ZST_ERROR;
        }

        // Configure demux location directly to the HTTPS URL
        const char* url = "https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_1MB.mp4";
        zst_element_set_property(demux, "location", url);

        // 3. Add to pipeline
        zst_pipeline_add(pipe, demux);

        // 4. Link pads
        // demux -> server (bunny_video, bunny_audio)
        char pad_name[128];
        snprintf(pad_name, sizeof(pad_name), "%s_video", name);
        zst_pad_link(zst_element_get_pad(demux, "video"), zst_element_get_pad(server, pad_name));

        snprintf(pad_name, sizeof(pad_name), "%s_audio", name);
        zst_pad_link(zst_element_get_pad(demux, "audio"), zst_element_get_pad(server, pad_name));

        // 5. Update pipeline execution sorting
        zst_pipeline_topological_sort(pipe);

        // 6. Set states — READY triggers avformat_open_input + find_stream_info
        zst_element_set_state(demux, ZST_STATE_READY);
        zst_element_set_state(demux, ZST_STATE_PLAYING);

        // 7. Pass avcC extradata to RTSP server for proper SDP generation
        //    (available immediately after READY because the file was probed).
        int extra_size = 0;
        const uint8_t* extra = zst_mp4_demuxer_get_video_extradata(demux, &extra_size);
        if (extra && extra_size > 0) {
            zst_rtsp_server_session_set_extradata(server, name, extra, extra_size);
            printf("[Demo App] Passed %d bytes of extradata to RTSP server for /%s\n",
                   extra_size, name);
        } else {
            fprintf(stderr, "[Demo App] Warning: no video extradata found for /%s — "
                            "SDP will use generic profile-level-id\n", name);
        }

        printf("[Demo App] Successfully mounted /%s HTTP source pipeline\n", name);
        return ZST_OK;
    }


    fprintf(stderr, "[Demo App] Unknown session requested: /%s\n", name);
    return ZST_ERROR;
}

int main(int argc, char** argv) {
    int port = 8554;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // Register built-in elements
    if (zst_register_builtin_elements() != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return 1;
    }

    // Create pipeline
    zst_pipeline_t* pipe = zst_pipeline_create();
    if (!pipe) {
        fprintf(stderr, "Failed to create pipeline\n");
        return 1;
    }
    zst_pipeline_set_clock_sync(pipe, 1);

    // Create RTSP server
    zst_element_t* server = zst_rtsp_server_create();
    if (!server) {
        fprintf(stderr, "Failed to create RTSP server\n");
        zst_pipeline_destroy(pipe);
        return 1;
    }

    // Configure port
    zst_element_set_property_int(server, "listen-port", port);

    // Set dynamic mount callback
    zst_rtsp_server_set_mount_callback(server, on_demand_mount, pipe);

    // Add server to pipeline
    zst_pipeline_add(pipe, server);

    // Create scheduler
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    if (!sched) {
        fprintf(stderr, "Failed to create scheduler\n");
        zst_pipeline_destroy(pipe);
        return 1;
    }

    // Attach pipeline and start
    zst_scheduler_attach(sched, pipe);
    zst_pipeline_set_state(pipe, ZST_STATE_READY);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    // Register SIGINT handler
    signal(SIGINT, sigint_handler);

    printf("\n=========================================================\n");
    printf("  RTSP Media-On-Demand Demo App Running on port %d\n", port);
    printf("=========================================================\n");
    printf("  Available streams:\n");
    printf("    rtsp://127.0.0.1:%d/colorbar\n", port);
    printf("    rtsp://127.0.0.1:%d/bunny\n", port);
    printf("\n  Press Ctrl+C to stop the server...\n");
    printf("=========================================================\n\n");

    // Listen to pipeline events (like errors) and run main loop
    while (g_running) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 200);
        if (r == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_ERROR) {
                fprintf(stderr, "[Pipeline Error] %s (%d)\n",
                        ev->as.error.message ? ev->as.error.message : "unknown",
                        (int)ev->as.error.result);
            }
            zst_event_destroy(ev);
        }
    }

    printf("\nStopping RTSP server...\n");

    // Clean up
    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    printf("Server stopped cleanly.\n");
    return 0;
}
