/*=============================================================================
    test_http_source.c — Test suite for the httpsrc (HTTP Source) element
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <time.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_http_source.h"

static int g_port = 15080;
static pthread_t g_server_thread;
static int g_server_running = 0;

static void
sleep_ms(unsigned int ms)
{
    struct timespec ts = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000UL)
    };
    nanosleep(&ts, NULL);
}

static void*
mock_http_server_thread(void* arg)
{
    int port = *(int*)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server_fd >= 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int bind_ret = bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    assert(bind_ret >= 0);

    int listen_ret = listen(server_fd, 10);
    assert(listen_ret >= 0);

    int resume_request_count = 0;
    g_server_running = 1;

    while (g_server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            break;
        }

        char req_buf[2048] = {0};
        int bytes_received = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
        if (bytes_received <= 0) {
            close(client_fd);
            continue;
        }
        fprintf(stderr, "--- MOCK SERVER RECEIVED REQUEST ---\n%s\n------------------------------------\n", req_buf);


        char path[128] = {0};
        sscanf(req_buf, "GET %127s", path);

        if (strcmp(path, "/normal") == 0) {
            const char* response = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 12\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Hello World!";
            send(client_fd, response, strlen(response), 0);
        } else if (strcmp(path, "/redirect") == 0) {
            char response[256];
            snprintf(response, sizeof(response),
                "HTTP/1.1 302 Found\r\n"
                "Location: http://127.0.0.1:%d/target\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n", port);
            send(client_fd, response, strlen(response), 0);
        } else if (strcmp(path, "/target") == 0) {
            const char* response = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: 10\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Redirected";
            send(client_fd, response, strlen(response), 0);
        } else if (strcmp(path, "/chunked") == 0) {
            const char* headers = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: video/mp4\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: close\r\n"
                "\r\n";
            send(client_fd, headers, strlen(headers), 0);

            const char* chunk1 = "5\r\nHello\r\n";
            send(client_fd, chunk1, strlen(chunk1), 0);

            const char* chunk2 = "7\r\n World!\r\n";
            send(client_fd, chunk2, strlen(chunk2), 0);

            const char* chunk_end = "0\r\n\r\n";
            send(client_fd, chunk_end, strlen(chunk_end), 0);
        } else if (strcmp(path, "/headers") == 0) {
            if (strstr(req_buf, "User-Agent: CustomUA") && strstr(req_buf, "Authorization: SecretToken")) {
                const char* response = 
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 7\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "Headers";
                send(client_fd, response, strlen(response), 0);
            } else {
                const char* response = 
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                send(client_fd, response, strlen(response), 0);
            }
        } else if (strcmp(path, "/resume") == 0) {
            char* range = strstr(req_buf, "Range: bytes=");
            unsigned long long offset = 0;
            if (range) {
                sscanf(range, "Range: bytes=%llu-", &offset);
            }

            if (offset == 5) {
                const char* response = 
                    "HTTP/1.1 206 Partial Content\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Range: bytes 5-9/10\r\n"
                    "Content-Length: 5\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "56789";
                send(client_fd, response, strlen(response), 0);
            } else {
                const char* headers = 
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 10\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "\r\n"
                    "01234";
                send(client_fd, headers, strlen(headers), 0);
            }
        }

        close(client_fd);
    }

    close(server_fd);
    return NULL;
}

static void
test_http_source_normal(void)
{
    printf("--- Test: http_source normal ---\n");
    zst_element_t* src = zst_http_source_create("");
    assert(src != NULL);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/normal", g_port);
    assert(zst_element_set_property(src, "url", url) == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);

    zst_pad_t* pad = zst_element_get_pad(src, "src");
    assert(pad != NULL);
    zst_caps_t* caps = zst_pad_get_caps(pad);
    assert(caps != NULL);
    assert(caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "text/plain") == 0);
    zst_caps_destroy(caps);

    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    char result_buf[256] = {0};
    size_t total_bytes = 0;
    while (1) {
        zst_buffer_t* out_buf = NULL;
        zst_result_t res = src->ops->process(src, NULL, &out_buf);
        if (res == ZST_EOF) {
            break;
        }
        assert(res == ZST_OK);
        assert(out_buf != NULL);
        memcpy(result_buf + total_bytes, out_buf->memory.data, out_buf->memory.size);
        total_bytes += out_buf->memory.size;
        zst_buffer_unref(out_buf);
    }

    assert(total_bytes == 12);
    assert(strcmp(result_buf, "Hello World!") == 0);

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(src);
}

static void
test_http_source_redirect(void)
{
    printf("--- Test: http_source redirect ---\n");
    zst_element_t* src = zst_http_source_create("");
    assert(src != NULL);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/redirect", g_port);
    assert(zst_element_set_property(src, "url", url) == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);

    zst_pad_t* pad = zst_element_get_pad(src, "src");
    assert(pad != NULL);
    zst_caps_t* caps = zst_pad_get_caps(pad);
    assert(caps != NULL);
    assert(caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "text/html") == 0);
    zst_caps_destroy(caps);

    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    char result_buf[256] = {0};
    size_t total_bytes = 0;
    while (1) {
        zst_buffer_t* out_buf = NULL;
        zst_result_t res = src->ops->process(src, NULL, &out_buf);
        if (res == ZST_EOF) {
            break;
        }
        assert(res == ZST_OK);
        assert(out_buf != NULL);
        memcpy(result_buf + total_bytes, out_buf->memory.data, out_buf->memory.size);
        total_bytes += out_buf->memory.size;
        zst_buffer_unref(out_buf);
    }

    assert(total_bytes == 10);
    assert(strcmp(result_buf, "Redirected") == 0);

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(src);
}

static void
test_http_source_chunked(void)
{
    printf("--- Test: http_source chunked ---\n");
    zst_element_t* src = zst_http_source_create("");
    assert(src != NULL);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/chunked", g_port);
    assert(zst_element_set_property(src, "url", url) == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);

    zst_pad_t* pad = zst_element_get_pad(src, "src");
    assert(pad != NULL);
    zst_caps_t* caps = zst_pad_get_caps(pad);
    assert(caps != NULL);
    assert(caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "video/mp4") == 0);
    zst_caps_destroy(caps);

    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    char result_buf[256] = {0};
    size_t total_bytes = 0;
    while (1) {
        zst_buffer_t* out_buf = NULL;
        zst_result_t res = src->ops->process(src, NULL, &out_buf);
        if (res == ZST_EOF) {
            break;
        }
        assert(res == ZST_OK);
        assert(out_buf != NULL);
        memcpy(result_buf + total_bytes, out_buf->memory.data, out_buf->memory.size);
        total_bytes += out_buf->memory.size;
        zst_buffer_unref(out_buf);
    }

    assert(total_bytes == 12);
    assert(strcmp(result_buf, "Hello World!") == 0);

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(src);
}

static void
test_http_source_headers(void)
{
    printf("--- Test: http_source custom headers ---\n");
    zst_element_t* src = zst_http_source_create("");
    assert(src != NULL);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/headers", g_port);
    assert(zst_element_set_property(src, "url", url) == ZST_OK);
    assert(zst_element_set_property(src, "user-agent", "CustomUA") == ZST_OK);
    assert(zst_element_set_property(src, "headers", "Authorization: SecretToken\r\n") == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    char result_buf[256] = {0};
    size_t total_bytes = 0;
    while (1) {
        zst_buffer_t* out_buf = NULL;
        zst_result_t res = src->ops->process(src, NULL, &out_buf);
        if (res == ZST_EOF) {
            break;
        }
        assert(res == ZST_OK);
        assert(out_buf != NULL);
        memcpy(result_buf + total_bytes, out_buf->memory.data, out_buf->memory.size);
        total_bytes += out_buf->memory.size;
        zst_buffer_unref(out_buf);
    }

    assert(total_bytes == 7);
    assert(strcmp(result_buf, "Headers") == 0);

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(src);
}

static void
test_http_source_resume(void)
{
    printf("--- Test: http_source resume / reconnect ---\n");
    zst_element_t* src = zst_http_source_create("");
    assert(src != NULL);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/resume", g_port);
    assert(zst_element_set_property(src, "url", url) == ZST_OK);
    assert(zst_element_set_property(src, "reconnect", "true") == ZST_OK);
    assert(zst_element_set_property(src, "reconnect-delay-ms", "50") == ZST_OK);
    assert(zst_element_set_property(src, "max-reconnect-attempts", "3") == ZST_OK);
    assert(zst_element_set_property(src, "chunk-size", "5") == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    char result_buf[256] = {0};
    size_t total_bytes = 0;
    while (1) {
        zst_buffer_t* out_buf = NULL;
        zst_result_t res = src->ops->process(src, NULL, &out_buf);
        if (res == ZST_EOF) {
            break;
        }
        assert(res == ZST_OK);
        assert(out_buf != NULL);
        memcpy(result_buf + total_bytes, out_buf->memory.data, out_buf->memory.size);
        total_bytes += out_buf->memory.size;
        zst_buffer_unref(out_buf);
    }

    assert(total_bytes == 10);
    assert(strcmp(result_buf, "0123456789") == 0);

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(src);
}

int main(void)
{
    printf("Starting HTTP Source test suite...\n");

    pthread_create(&g_server_thread, NULL, mock_http_server_thread, &g_port);
    sleep_ms(100);

    test_http_source_normal();
    test_http_source_redirect();
    test_http_source_chunked();
    test_http_source_headers();
    test_http_source_resume();

    g_server_running = 0;
    int dummy_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(g_port);
    connect(dummy_fd, (struct sockaddr*)&addr, sizeof(addr));
    close(dummy_fd);

    pthread_join(g_server_thread, NULL);

    printf("All HTTP Source tests passed!\n");
    return 0;
}
