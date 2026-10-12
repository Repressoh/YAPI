// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define main payload_main
#include "main.c"
#undef main

#include <assert.h>
#include <stdarg.h>

#define TEST_ID "XX0000-CUSA00001_00-ABCDEFGHIJKLMNOP"
#define TEST_PREFIX "/v1/" TEST_ID "/0123456789abcdef0123456789abcdef/"
#define TEST_ENCODED "aHR0cHM6Ly9leGFtcGxlLmludmFsaWQvcGtnP3Rva2VuPWFiYw"
#define TEST_PATH TEST_PREFIX TEST_ENCODED "/pkg"
#define OTHER_PATH TEST_PREFIX "aHR0cHM6Ly9leGFtcGxlLmludmFsaWQvbmV4dC5wa2c/pkg"

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool release_second_chunk;
static int read_calls;
static int upstream_requests;
static int upstream_status = 200;
static int upstream_length_type;
static int second_read_result = 3;
static const char *upstream_content_range = "bytes 0-5/6";
static char forwarded_range[128];
static const char *expected_url = "https://example.invalid/pkg?token=abc";

/* Parentheses stop main.c's klog_printf macro from expanding here. */
int (klog_printf)(const char *fmt, ...) { (void)fmt; return 0; }
int sceKernelSendNotificationRequest(int device, Notification *req, size_t size, int blocking) {
    (void)device; (void)req; (void)size; (void)blocking; return 0;
}
int sceHttpCreateTemplate(int ctx, const char *agent, int version, int proxy) {
    (void)ctx; (void)agent; (void)version; (void)proxy; return 1;
}
int sceHttpDeleteTemplate(int id) { (void)id; return 0; }
int sceHttpSetAutoRedirect(int id, int enabled) { (void)id; (void)enabled; return 0; }
int sceHttpSetResolveTimeOut(int id, uint32_t timeout) { (void)id; (void)timeout; return 0; }
int sceHttpSetConnectTimeOut(int id, uint32_t timeout) { (void)id; (void)timeout; return 0; }
int sceHttpSetSendTimeOut(int id, uint32_t timeout) { (void)id; (void)timeout; return 0; }
int sceHttpSetRecvTimeOut(int id, uint32_t timeout) { (void)id; (void)timeout; return 0; }
int sceHttpsSetSslCallback(int id, SslCallback callback, void *user) {
    (void)id; (void)user; assert(callback(0, 0x20, NULL, 0, NULL) >= 0); return 0;
}
int sceHttpCreateConnectionWithURL(int tmpl, const char *url, int keepalive) {
    (void)tmpl; (void)keepalive;
    ++upstream_requests;
    assert(strcmp(url, expected_url) == 0);
    return 2;
}
int sceHttpDeleteConnection(int id) { (void)id; return 0; }
int sceHttpCreateRequestWithURL(int conn, int method, const char *url, uint64_t length) {
    (void)conn; (void)length;
    assert(strcmp(url, expected_url) == 0);
    assert(method == 0);
    return 3;
}
int sceHttpDeleteRequest(int id) { (void)id; return 0; }
int sceHttpAddRequestHeader(int id, const char *name, const char *value, int mode) {
    (void)id; (void)mode;
    if (strcmp(name, "Range") == 0) snprintf(forwarded_range, sizeof(forwarded_range), "%s", value);
    return 0;
}
int sceHttpSendRequest(int id, const void *data, size_t size) {
    (void)id; (void)data; (void)size; return 0;
}
int sceHttpGetLastErrno(int id, int *error) { (void)id; *error = 0; return 0; }
int sceHttpGetStatusCode(int id, int *status) { (void)id; *status = upstream_status; return 0; }
int sceHttpGetResponseContentLength(int id, int *type, size_t *length) {
    (void)id; *type = upstream_length_type; *length = 6; return 0;
}
int sceHttpGetAllResponseHeaders(int id, char **headers, size_t *length) {
    (void)id;
    static char response[256];
    snprintf(response, sizeof(response), "Content-Length: 6\r\n");
    if (upstream_status == 206 || upstream_status == 416)
        snprintf(response, sizeof(response), "Content-Length: 6\r\nContent-Range: %s\r\n", upstream_content_range);
    *headers = response;
    *length = strlen(*headers);
    return 0;
}
int sceHttpReadData(int id, void *buffer, uint32_t capacity) {
    (void)id; assert(capacity >= 3);
    if (read_calls++ == 0) { memcpy(buffer, "abc", 3); return 3; }
    if (read_calls == 2) {
        if (second_read_result <= 0) return second_read_result;
        pthread_mutex_lock(&gate_mutex);
        while (!release_second_chunk) pthread_cond_wait(&gate_cond, &gate_mutex);
        pthread_mutex_unlock(&gate_mutex);
        memcpy(buffer, "def", 3);
        return 3;
    }
    return 0;
}

static void *serve_thread(void *argument) {
    int fd = (int)(intptr_t)argument;
    serve_package(fd);
    close(fd);
    return NULL;
}

static void request_reply(const char *request, char *reply, size_t capacity) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct timeval timeout = {2, 0};
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    read_calls = 0;
    forwarded_range[0] = 0;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, serve_thread, (void *)(intptr_t)sockets[1]) == 0);
    assert(send_all(sockets[0], request, strlen(request)));
    size_t used = 0;
    for (;;) {
        ssize_t n = recv(sockets[0], reply + used, capacity - 1 - used, 0);
        assert(n >= 0);
        if (n == 0) break;
        used += (size_t)n;
        assert(used < capacity - 1);
    }
    reply[used] = 0;
    pthread_join(thread, NULL);
    close(sockets[0]);
}

int main(void) {
    char encoded[MAX_ENCODED_URL + 1], decoded[MAX_URL];
    assert(encode_url(expected_url, encoded, sizeof(encoded)));
    assert(strcmp(encoded, TEST_ENCODED) == 0);
    assert(decode_url(TEST_ENCODED, strlen(TEST_ENCODED), decoded, sizeof(decoded)));
    assert(strcmp(decoded, expected_url) == 0);
    assert(!decode_url(TEST_ENCODED, strlen(TEST_ENCODED), decoded, strlen(expected_url)));
    assert(!encode_url(expected_url, encoded, strlen(TEST_ENCODED)));
    const char *invalid_paths[] = {
        "/1/pkg", "/v2/" TEST_ID "/0123456789abcdef0123456789abcdef/" TEST_ENCODED "/pkg",
        "/v1/invalid/0123456789abcdef0123456789abcdef/" TEST_ENCODED "/pkg",
        "/v1/" TEST_ID "/1/" TEST_ENCODED "/pkg",
        "/v1/" TEST_ID "/g123456789abcdef0123456789abcdef/" TEST_ENCODED "/pkg",
        TEST_PREFIX "/pkg", TEST_PREFIX "a/pkg", TEST_PREFIX "====/pkg",
        TEST_PREFIX "aHR0cHM6Ly94LwA/pkg", /* Encoded NUL. */
        TEST_PREFIX "aHR0cHM6Ly94Lwo/pkg", /* Encoded newline. */
        TEST_PREFIX "aHR0cHM6Ly94LyNmcmFnbWVudA/pkg", /* Fragment. */
        TEST_PREFIX "ZmlsZTovLy94LnBrZw/pkg", /* Unsupported scheme. */
        TEST_PREFIX "aHR0cHM6Ly9leGFtcGxlLmludmFsaWQvcGtnP3Rva2VuPWFiYx/pkg", /* Nonzero padding bits. */
        TEST_PREFIX TEST_ENCODED "=/pkg", TEST_PATH "/../1.pkg", TEST_PATH "/a.pkg",
        TEST_PREFIX TEST_ENCODED "/other", TEST_PREFIX TEST_ENCODED "/pkg/1.pkg/extra"
    };
    char malformed[MAX_REQUEST], invalid_reply[1024];
    for (size_t i = 0; i < sizeof(invalid_paths) / sizeof(invalid_paths[0]); ++i) {
        int before = upstream_requests;
        snprintf(malformed, sizeof(malformed), "GET %s HTTP/1.1\r\n\r\n", invalid_paths[i]);
        request_reply(malformed, invalid_reply, sizeof(invalid_reply));
        assert(strstr(invalid_reply, "HTTP/1.1 400 Bad Request") && upstream_requests == before);
    }
    assert(range_matches("bytes=-6", "bytes 94-99/100", 0, 6));
    assert(range_matches("bytes=-10", "bytes 0-5/6", 0, 6));
    assert(range_matches("bytes=-1", "bytes 99-99/100", 0, 1));
    assert(!range_matches("bytes=-6", "bytes 0-5/100", 0, 6));
    assert(!range_matches("bytes=-6", "bytes 95-99/100", 0, 5));
    assert(!range_matches("bytes=-6", "bytes 94-99/*", 0, 6));
    assert(!range_matches("bytes=-0", "bytes 0-5/6", 0, 6));
    assert(range_matches("bytes=0-5", "bytes 0-5/6 \t", 0, 6));
    assert(range_matches("bytes=0-5", "bytes 0-5/*", 0, 6));
    assert(range_matches("bytes=10-", "bytes 10-15/100", 0, 6));
    assert(!range_matches("bytes=0-3", "bytes 0-5/6", 0, 6));
    assert(!range_matches("bytes=1-6", "bytes 0-5/6", 0, 6));
    assert(!range_matches("bytes=0-5", "bytes 0-5/6", 0, 5));
    const char *invalid_ranges[] = {
        "bytes 0-5/not-a-total", "bytes 0-5/0", "bytes 0-5/5",
        "bytes 0-5/6junk", "bytes 0-5/*junk", "bytes +0-5/6",
        "bytes 0-+5/6", "bytes 0-5/+6", "bytes 0-5/18446744073709551616",
        "bytes 0-18446744073709551615/*", "bytes 5-0/6"
    };
    for (size_t i = 0; i < sizeof(invalid_ranges) / sizeof(invalid_ranges[0]); ++i)
        assert(!range_matches("bytes=0-5", invalid_ranges[i], 0, 6));

    http_ctx = 1;
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, serve_thread, (void *)(intptr_t)sockets[1]) == 0);
    const char request[] = "GET " TEST_PATH "/2.pkg HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    assert(send_all(sockets[0], request, sizeof(request) - 1));
    struct timeval timeout = {2, 0};
    setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char reply[1024] = {0};
    size_t used = 0;
    while (!strstr(reply, "\r\n\r\n") || !strstr(reply, "abc")) {
        ssize_t n = recv(sockets[0], reply + used, sizeof(reply) - used - 1, 0);
        assert(n > 0);
        used += (size_t)n;
        reply[used] = 0;
    }
    assert(strstr(reply, "HTTP/1.1 200 OK") != NULL);
    assert(forwarded_range[0] == 0);
    // The worker streams independently of any control command or route registry.
    pthread_mutex_lock(&gate_mutex);
    release_second_chunk = true;
    pthread_cond_signal(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    pthread_join(thread, NULL);
    while (used + 1 < sizeof(reply)) {
        ssize_t n = recv(sockets[0], reply + used, sizeof(reply) - used - 1, 0);
        if (n <= 0) break;
        used += (size_t)n;
        reply[used] = 0;
    }
    assert(strstr(reply, "abcdef") != NULL);
    close(sockets[0]);
    const char range_request[] = "GET " TEST_PATH "/3.pkg HTTP/1.1\r\nRange: bytes=0-5\r\n\r\n";
    upstream_status = 206;
    read_calls = 0;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(pthread_create(&thread, NULL, serve_thread, (void *)(intptr_t)sockets[1]) == 0);
    assert(send_all(sockets[0], range_request, sizeof(range_request) - 1));
    used = 0;
    while (used + 1 < sizeof(reply)) {
        ssize_t count = recv(sockets[0], reply + used, sizeof(reply) - used - 1, 0);
        if (count <= 0) break;
        used += (size_t)count;
        reply[used] = 0;
    }
    assert(strstr(reply, "HTTP/1.1 206 Partial Content") != NULL);
    assert(strstr(reply, "Content-Range: bytes 0-5/6") != NULL);
    assert(strstr(reply, "abcdef") != NULL);
    assert(strcmp(forwarded_range, "bytes=0-5") == 0);
    pthread_join(thread, NULL);
    close(sockets[0]);

    upstream_status = 200;
    forwarded_range[0] = 0;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(pthread_create(&thread, NULL, serve_thread, (void *)(intptr_t)sockets[1]) == 0);
    assert(send_all(sockets[0], range_request, sizeof(range_request) - 1));
    used = 0;
    ssize_t n = recv(sockets[0], reply, sizeof(reply) - 1, 0);
    assert(n > 0);
    reply[n] = 0;
    assert(strstr(reply, "HTTP/1.1 502 Bad Gateway") != NULL);
    assert(strcmp(forwarded_range, "bytes=0-5") == 0);
    pthread_join(thread, NULL);
    close(sockets[0]);
    const char suffix_request[] = "GET " TEST_PATH " HTTP/1.1\r\nRange: bytes=-6\r\n\r\n";
    upstream_status = 206;
    upstream_content_range = "bytes 94-99/100";
    request_reply(suffix_request, reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 206 Partial Content") && strstr(reply, "abcdef"));
    assert(strcmp(forwarded_range, "bytes=-6") == 0);
    upstream_content_range = "bytes 0-5/100";
    request_reply(suffix_request, reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 502 Bad Gateway") && read_calls == 0);
    upstream_content_range = "bytes 0-5/not-a-total";
    request_reply(range_request, reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 502 Bad Gateway") && read_calls == 0);

    upstream_status = 200;
    request_reply("HEAD " TEST_PATH " HTTP/1.1\r\n\r\n", reply, sizeof(reply));
    assert(strstr(reply, "Content-Length: 6\r\n") && read_calls == 0);
    assert(strstr(reply, "\r\n\r\n")[4] == 0);
    expected_url = "https://example.invalid/next.pkg";
    request_reply("HEAD " OTHER_PATH "?product=test HTTP/1.1\r\n\r\n", reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 200 OK") && read_calls == 0);
    request_reply("GET /20/pkg HTTP/1.1\r\n\r\n", reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 400 Bad Request") && read_calls == 0);
    expected_url = "https://example.invalid/pkg?token=abc";
    upstream_status = 416;
    upstream_content_range = "bytes */6";
    request_reply(range_request, reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 416 Range Not Satisfiable"));
    assert(strstr(reply, "Content-Range: bytes */6\r\n") && strstr(reply, "Content-Length: 0\r\n"));
    assert(read_calls == 0 && strstr(reply, "\r\n\r\n")[4] == 0);

    upstream_status = 200;
    upstream_length_type = 1;
    request_reply(request, reply, sizeof(reply));
    assert(strstr(reply, "Transfer-Encoding: chunked\r\n"));
    assert(strstr(reply, "\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n"));
    second_read_result = -1;
    request_reply(request, reply, sizeof(reply));
    assert(strstr(reply, "\r\n\r\n3\r\nabc\r\n") && !strstr(reply, "0\r\n\r\n"));
    upstream_length_type = 0;
    second_read_result = 0;
    request_reply(request, reply, sizeof(reply));
    assert(strstr(reply, "Content-Length: 6\r\n") && strcmp(strstr(reply, "\r\n\r\n") + 4, "abc") == 0);
    /* A decoded signed URL is preserved exactly; local PlayGo metadata is ignored. */
    expected_url = "https://example.invalid/pkg?sig=A%2FB%3D&name=Package%20Update";
    PackageRequest signed_package = {0};
    strcpy(signed_package.remote_url, expected_url);
    strcpy(signed_package.content_id, TEST_ID);
    char local_url[MAX_LOCAL_URL];
    assert(make_local_url(&signed_package, local_url, sizeof(local_url)));
    snprintf(malformed, sizeof(malformed), "HEAD %s/4.pkg?product=test&serverIpAddr=127.0.0.1&r=1 HTTP/1.1\r\n\r\n",
             local_url + strlen(LOCAL_ORIGIN));
    request_reply(malformed, reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 200 OK") && read_calls == 0);

    /* Exercise the longest supported URL through the HTTP parser as well. */
    char longest[MAX_URL];
    strcpy(longest, "https://example.invalid/");
    size_t prefix_length = strlen(longest);
    memset(longest + prefix_length, 'a', sizeof(longest) - prefix_length - 1);
    longest[sizeof(longest) - 1] = 0;
    strcpy(signed_package.remote_url, longest);
    expected_url = longest;
    assert(make_local_url(&signed_package, local_url, sizeof(local_url)));
    snprintf(malformed, sizeof(malformed), "HEAD %s HTTP/1.1\r\n\r\n", local_url + strlen(LOCAL_ORIGIN));
    request_reply(malformed, reply, sizeof(reply));
    assert(strstr(reply, "HTTP/1.1 200 OK") && read_calls == 0);
    puts("PASS: stateless routes, signed URLs, malformed paths, URL limits, early streaming, ranges, HEAD/416, chunked/truncated streams");
    return 0;
}
