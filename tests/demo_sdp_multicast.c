/*=============================================================================
    demo_sdp_multicast.c — SDP mux/demux RTP multicast demo

    Terminal 1:
      ./demo_sdp_multicast send /tmp/zstreamer-demo.sdp 239.255.42.42 10

    Terminal 2:
      ./demo_sdp_multicast recv /tmp/zstreamer-demo.sdp 239.255.42.42 10

    Sender pipeline (manual demo driver):
      videotestsrc + audiotestsrc -> x264enc + aacenc -> RTP multicast
      sdpmuxer observes encoded packets and writes the SDP file.

    Receiver pipeline:
      UDP multicast sockets -> sdpdemuxer(sdp-file=...) -> fakesink(s)
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_aac_encoder.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zstreamer/elements/zst_sdp_demuxer.h"
#include "zstreamer/elements/zst_sdp_muxer.h"
#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_x264_encoder.h"

#define DEMO_VIDEO_PORT 5004
#define DEMO_AUDIO_PORT 5006
#define DEMO_VIDEO_PT   96
#define DEMO_AUDIO_PT   97
#define DEMO_MTU        1200
#define DEMO_VIDEO_SSRC 0x53545056u /* STPV */
#define DEMO_AUDIO_SSRC 0x53545041u /* STPA */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t target_ns) {
    while (!g_stop) {
        uint64_t n = now_ns();
        if (n >= target_ns) return;
        uint64_t diff = target_ns - n;
        struct timespec ts;
        ts.tv_sec = (time_t)(diff / 1000000000ULL);
        ts.tv_nsec = (long)(diff % 1000000000ULL);
        nanosleep(&ts, NULL);
    }
}

typedef struct {
    int fd;
    struct sockaddr_in addr;
    uint16_t seq;
    uint32_t ssrc;
    uint8_t pt;
    uint32_t clock_rate;
} rtp_sender_t;

static int make_sender(rtp_sender_t* s, const char* group, int port,
                       uint8_t pt, uint32_t ssrc, uint32_t clock_rate) {
    memset(s, 0, sizeof(*s));
    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) {
        perror("socket");
        return -1;
    }

    int ttl = 16;
    setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    unsigned char loop = 1;
    setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    s->addr.sin_family = AF_INET;
    s->addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, group, &s->addr.sin_addr) != 1) {
        fprintf(stderr, "invalid multicast group: %s\n", group);
        close(s->fd);
        return -1;
    }
    s->seq = 0x7000; /* keep sdpdemux single-packet raw-RTP autodetect clamped */
    s->ssrc = ssrc;
    s->pt = pt;
    s->clock_rate = clock_rate;
    return 0;
}

static int send_rtp(rtp_sender_t* s, const uint8_t* payload, int payload_len,
                    uint32_t rtp_ts, int marker) {
    uint8_t pkt[DEMO_MTU + 32];
    if (!s || !payload || payload_len < 0 || payload_len > DEMO_MTU) return -1;
    pkt[0] = 0x80;
    pkt[1] = (uint8_t)((marker ? 0x80 : 0x00) | (s->pt & 0x7f));
    pkt[2] = (uint8_t)(s->seq >> 8);
    pkt[3] = (uint8_t)(s->seq & 0xff);
    pkt[4] = (uint8_t)(rtp_ts >> 24);
    pkt[5] = (uint8_t)(rtp_ts >> 16);
    pkt[6] = (uint8_t)(rtp_ts >> 8);
    pkt[7] = (uint8_t)(rtp_ts);
    pkt[8] = (uint8_t)(s->ssrc >> 24);
    pkt[9] = (uint8_t)(s->ssrc >> 16);
    pkt[10] = (uint8_t)(s->ssrc >> 8);
    pkt[11] = (uint8_t)(s->ssrc);
    memcpy(pkt + 12, payload, (size_t)payload_len);
    ssize_t n = sendto(s->fd, pkt, (size_t)payload_len + 12, 0,
                       (struct sockaddr*)&s->addr, sizeof(s->addr));
    s->seq++;
    return n >= 0 ? 0 : -1;
}

static int find_start_code(const uint8_t* data, int size, int offset, int* code_size) {
    for (int i = offset; i + 3 <= size; i++) {
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            *code_size = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *code_size = 3;
            return i;
        }
    }
    return -1;
}

static int packetize_h264(rtp_sender_t* s, const uint8_t* data, int size, uint64_t pts_ns) {
    uint32_t ts = (uint32_t)(pts_ns * 90000ULL / 1000000000ULL);
    int code = 0;
    int pos = find_start_code(data, size, 0, &code);
    if (pos < 0) {
        return send_rtp(s, data, size, ts, 1);
    }

    while (pos >= 0 && pos < size) {
        int nal_start = pos + code;
        int next_code = 0;
        int next = find_start_code(data, size, nal_start, &next_code);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_len = nal_end - nal_start;
        int is_last = next < 0;
        if (nal_len > 0) {
            const uint8_t* nal = data + nal_start;
            if (nal_len <= DEMO_MTU) {
                send_rtp(s, nal, nal_len, ts, is_last);
            } else {
                uint8_t fu_ind = (uint8_t)((nal[0] & 0xe0) | 28);
                uint8_t nal_type = nal[0] & 0x1f;
                int off = 1;
                int first = 1;
                while (off < nal_len) {
                    int chunk = nal_len - off;
                    if (chunk > DEMO_MTU - 2) chunk = DEMO_MTU - 2;
                    uint8_t fu[DEMO_MTU];
                    fu[0] = fu_ind;
                    fu[1] = nal_type;
                    if (first) fu[1] |= 0x80;
                    if (off + chunk >= nal_len) fu[1] |= 0x40;
                    memcpy(fu + 2, nal + off, (size_t)chunk);
                    send_rtp(s, fu, chunk + 2, ts, is_last && (off + chunk >= nal_len));
                    off += chunk;
                    first = 0;
                }
            }
        }
        if (next < 0) break;
        pos = next;
        code = next_code;
    }
    return 0;
}

static int packetize_aac(rtp_sender_t* s, const uint8_t* data, int size, uint64_t pts_ns) {
    if (size <= 0 || size > 0x1fff) return -1;
    uint8_t payload[DEMO_MTU + 8];
    if (size + 4 > (int)sizeof(payload)) return -1;
    uint16_t au_header = (uint16_t)(size << 3);
    payload[0] = 0;
    payload[1] = 16; /* one 16-bit AU-header */
    payload[2] = (uint8_t)(au_header >> 8);
    payload[3] = (uint8_t)(au_header & 0xff);
    memcpy(payload + 4, data, (size_t)size);
    uint32_t ts = (uint32_t)(pts_ns * s->clock_rate / 1000000000ULL);
    return send_rtp(s, payload, size + 4, ts, 1);
}

static int write_sdp(zst_element_t* sdpmux, const char* path) {
    char sdp[4096];
    if (zst_element_get_property(sdpmux, "sdp", sdp, sizeof(sdp)) != ZST_OK) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    fwrite(sdp, 1, strlen(sdp), f);
    fclose(f);
    printf("Wrote SDP to %s:\n%s\n", path, sdp);
    return 0;
}

static int run_sender(const char* sdp_path, const char* group, int seconds) {
    zst_element_t* vsrc = zst_video_test_src_create();
    zst_element_t* asrc = zst_audio_test_src_create();
    zst_element_t* venc = zst_x264_encoder_create();
    zst_element_t* aenc = zst_aac_encoder_create();
    zst_element_t* sdpmux = zst_sdp_muxer_create();
    if (!vsrc || !asrc || !venc || !aenc || !sdpmux) {
        fprintf(stderr, "failed to create elements\n");
        return 1;
    }

    zst_element_set_property(vsrc, "width", "640");
    zst_element_set_property(vsrc, "height", "360");
    zst_element_set_property(vsrc, "fps", "30");
    zst_element_set_property(venc, "fps", "30/1");
    zst_element_set_property(venc, "preset", "ultrafast");
    zst_element_set_property(venc, "tune", "zerolatency");
    zst_element_set_property(asrc, "sample-rate", "48000");
    zst_element_set_property(asrc, "channels", "2");
    zst_element_set_property(asrc, "samples-per-buffer", "1024");
    zst_element_set_property(aenc, "sample-rate", "48000");
    zst_element_set_property(aenc, "channels", "2");
    zst_element_set_property(sdpmux, "address", group);
    zst_element_set_property(sdpmux, "enable-audio", "true");
    zst_element_set_property(sdpmux, "sample-rate", "48000");
    zst_element_set_property(sdpmux, "channels", "2");
    zst_element_set_property(sdpmux, "video-port", "5004");
    zst_element_set_property(sdpmux, "audio-port", "5006");

    zst_element_set_state(vsrc, ZST_STATE_PLAYING);
    zst_element_set_state(asrc, ZST_STATE_PLAYING);
    zst_element_set_state(venc, ZST_STATE_PLAYING);
    zst_element_set_state(aenc, ZST_STATE_PLAYING);
    zst_element_set_state(sdpmux, ZST_STATE_PLAYING);

    rtp_sender_t vs, as;
    if (make_sender(&vs, group, DEMO_VIDEO_PORT, DEMO_VIDEO_PT, DEMO_VIDEO_SSRC, 90000) < 0 ||
        make_sender(&as, group, DEMO_AUDIO_PORT, DEMO_AUDIO_PT, DEMO_AUDIO_SSRC, 48000) < 0) {
        return 1;
    }

    zst_pad_t* sdpmux_video = zst_element_get_pad(sdpmux, "video");
    zst_pad_t* sdpmux_audio = zst_element_get_pad(sdpmux, "audio");
    int wrote_sdp = 0;
    uint64_t start = now_ns();
    uint64_t end = start + (uint64_t)seconds * 1000000000ULL;
    uint64_t next_v = start;
    uint64_t next_a = start;
    uint64_t v_period = 1000000000ULL / 30ULL;
    uint64_t a_period = 1024ULL * 1000000000ULL / 48000ULL;
    uint64_t v_pkts = 0, a_pkts = 0;

    printf("Sending RTP multicast to %s video:%d audio:%d for %d sec\n",
           group, DEMO_VIDEO_PORT, DEMO_AUDIO_PORT, seconds);

    while (!g_stop && now_ns() < end) {
        uint64_t n = now_ns();
        if (n >= next_v) {
            zst_buffer_t *raw = NULL, *enc = NULL;
            if (vsrc->ops->process(vsrc, NULL, &raw) == ZST_OK && raw) {
                if (venc->ops->process(venc, raw, &enc) == ZST_OK && enc) {
                    if (sdpmux_video) sdpmux_video->push(sdpmux_video, enc);
                    if (!wrote_sdp) {
                        write_sdp(sdpmux, sdp_path);
                        wrote_sdp = 1;
                    }
                    packetize_h264(&vs, (const uint8_t*)enc->memory.data, (int)enc->memory.size, enc->pts);
                    v_pkts++;
                    zst_buffer_unref(enc);
                }
                zst_buffer_unref(raw);
            }
            next_v += v_period;
        }
        if (n >= next_a) {
            zst_buffer_t *raw = NULL, *enc = NULL;
            if (asrc->ops->process(asrc, NULL, &raw) == ZST_OK && raw) {
                if (aenc->ops->process(aenc, raw, &enc) == ZST_OK && enc) {
                    if (sdpmux_audio) sdpmux_audio->push(sdpmux_audio, enc);
                    if (!wrote_sdp) {
                        write_sdp(sdpmux, sdp_path);
                        wrote_sdp = 1;
                    }
                    packetize_aac(&as, (const uint8_t*)enc->memory.data, (int)enc->memory.size, enc->pts);
                    a_pkts++;
                    zst_buffer_unref(enc);
                }
                zst_buffer_unref(raw);
            }
            next_a += a_period;
        }
        uint64_t next = next_v < next_a ? next_v : next_a;
        sleep_until(next);
    }

    printf("Sender done: video access-units=%llu audio access-units=%llu\n",
           (unsigned long long)v_pkts, (unsigned long long)a_pkts);
    close(vs.fd);
    close(as.fd);
    zst_element_destroy(vsrc);
    zst_element_destroy(asrc);
    zst_element_destroy(venc);
    zst_element_destroy(aenc);
    zst_element_destroy(sdpmux);
    return 0;
}

static int make_receiver(const char* group, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
        close(fd);
        return -1;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("IP_ADD_MEMBERSHIP");
        close(fd);
        return -1;
    }
    return fd;
}

static zst_pad_t* find_src_pad_prefix(zst_element_t* el, const char* prefix) {
    if (!el || !prefix) return NULL;
    size_t n = strlen(prefix);
    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        if (strncmp(el->src_pads[i]->name, prefix, n) == 0) return el->src_pads[i];
    }
    return NULL;
}

static int run_receiver(const char* sdp_path, const char* group, int seconds) {
    zst_element_t* demux = zst_sdp_demuxer_create();
    zst_element_t* vfakesink = zst_fake_sink_create();
    zst_element_t* afakesink = zst_fake_sink_create();
    if (!demux || !vfakesink || !afakesink) return 1;

    zst_element_set_property(demux, "sdp-file", sdp_path);
    zst_element_set_state(demux, ZST_STATE_PLAYING);
    zst_element_set_property(vfakesink, "push-per-second", "true");
    zst_element_set_property(afakesink, "push-per-second", "true");
    zst_element_set_property(vfakesink, "log-period", "1");
    zst_element_set_property(afakesink, "log-period", "1");
    zst_element_set_state(vfakesink, ZST_STATE_PLAYING);
    zst_element_set_state(afakesink, ZST_STATE_PLAYING);

    zst_pad_t* vsrc = find_src_pad_prefix(demux, "video_");
    zst_pad_t* asrc = find_src_pad_prefix(demux, "audio_");
    if (vsrc) zst_pad_link(vsrc, zst_element_get_pad(vfakesink, "sink"));
    if (asrc) zst_pad_link(asrc, zst_element_get_pad(afakesink, "sink"));

    int vfd = make_receiver(group, DEMO_VIDEO_PORT);
    int afd = make_receiver(group, DEMO_AUDIO_PORT);
    if (vfd < 0 || afd < 0) return 1;

    zst_pad_t* sink = zst_element_get_pad(demux, "sink");
    uint64_t start = now_ns();
    uint64_t end = start + (uint64_t)seconds * 1000000000ULL;
    uint64_t last_print = start;
    uint64_t last_v = 0, last_a = 0;
    char val[64];

    printf("Receiving RTP multicast from %s video:%d audio:%d using %s\n",
           group, DEMO_VIDEO_PORT, DEMO_AUDIO_PORT, sdp_path);

    while (!g_stop && now_ns() < end) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(vfd, &rfds);
        FD_SET(afd, &rfds);
        int maxfd = vfd > afd ? vfd : afd;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0) {
            for (int which = 0; which < 2; which++) {
                int fd = which == 0 ? vfd : afd;
                if (!FD_ISSET(fd, &rfds)) continue;
                uint8_t pkt[1600];
                ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
                if (n <= 0) continue;
                zst_buffer_t* b = zst_buffer_create(ZST_BUFFER_USER);
                if (!b) continue;
                uint8_t* copy = malloc((size_t)n);
                if (!copy) {
                    zst_buffer_unref(b);
                    continue;
                }
                memcpy(copy, pkt, (size_t)n);
                b->memory.data = copy;
                b->memory.size = (size_t)n;
                b->memory.priv = copy;
                b->memory.release = free;
                sink->push(sink, b);
                zst_buffer_unref(b);
            }
        }

        uint64_t n = now_ns();
        if (n - last_print >= 1000000000ULL) {
            uint64_t vtotal = 0, atotal = 0;
            if (zst_element_get_property(vfakesink, "total-buffers", val, sizeof(val)) == ZST_OK) vtotal = strtoull(val, NULL, 10);
            if (zst_element_get_property(afakesink, "total-buffers", val, sizeof(val)) == ZST_OK) atotal = strtoull(val, NULL, 10);
            printf("FPS: video=%.1f audio-packets=%.1f totals: video=%llu audio=%llu\n",
                   (double)(vtotal - last_v), (double)(atotal - last_a),
                   (unsigned long long)vtotal, (unsigned long long)atotal);
            last_v = vtotal;
            last_a = atotal;
            last_print = n;
        }
    }

    close(vfd);
    close(afd);
    zst_element_destroy(demux);
    zst_element_destroy(vfakesink);
    zst_element_destroy(afakesink);
    return 0;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s send [sdp-file] [multicast-group] [seconds]\n"
            "  %s recv [sdp-file] [multicast-group] [seconds]\n"
            "\nDefaults: sdp-file=/tmp/zstreamer-demo.sdp group=239.255.42.42 seconds=10\n",
            argv0, argv0);
}

int main(int argc, char** argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const char* mode = argc > 1 ? argv[1] : NULL;
    const char* sdp = argc > 2 ? argv[2] : "/tmp/zstreamer-demo.sdp";
    const char* group = argc > 3 ? argv[3] : "239.255.42.42";
    int seconds = argc > 4 ? atoi(argv[4]) : 10;
    if (seconds <= 0) seconds = 10;

    if (!mode) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(mode, "send") == 0) return run_sender(sdp, group, seconds);
    if (strcmp(mode, "recv") == 0) return run_receiver(sdp, group, seconds);

    usage(argv[0]);
    return 2;
}
