// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PS5_PROXY_HOST_TEST
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef PS5_PROXY_HOST_TEST
int klog_printf(const char *fmt, ...);
#else
#include <ps5/klog.h>
#endif

#define CONTROL_PORT 54309
#define PROXY_PORT 54310
#define MAX_URL 4096
#define MAX_TASK_NAME 256
/* ShellCore reports this code for an invalid application slot. */
#define INSTALL_INVALID_SLOT_ERROR ((int32_t)UINT32_C(0x80B2116F))
#define INSTALL_MAX_ATTEMPTS 3
#define INSTALL_RETRY_SECONDS 2
#define MAX_REQUEST 8192
#define CHUNK_SIZE 65536
#define MAX_WORKERS 8
#define WORKER_STACK_SIZE (512 * 1024)
#define LOCAL_ORIGIN "http://127.0.0.1:54310"
#define CONTENT_ID_SIZE 48
#define REQUEST_ID_LENGTH 32
#define MAX_ENCODED_URL (((MAX_URL - 1) * 4 + 2) / 3)
#define MAX_LOCAL_URL (sizeof(LOCAL_ORIGIN) + 4 + CONTENT_ID_SIZE + 1 + \
                       REQUEST_ID_LENGTH + 1 + MAX_ENCODED_URL + 4)

/* AppInstUtil layouts from phantomptr/ps5upload, payload/include/sceAppInstUtil.h.
 * PackageInfo is the local name for SceAppInstallPkgInfo in that header. */
typedef struct {
    char content_id[0x30];
    int content_type;
    int content_platform;
} PackageInfo;

typedef struct {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
    uint64_t    unknown_30;     /* read by Sony; must be 0 for a plain install */
    uint64_t    reserved[7];    /* keep zero */
} MetaInfo;

typedef struct {
    char languages[30][8];
    char playgo_scenario_ids[64][3];
    char content_ids[64][0x30];
    unsigned char unknown[6480];
} PlayGoInfo;

typedef struct {
    char reserved[45];
    char message[3075];
} Notification;

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilInstallByPackage(MetaInfo *, PackageInfo *, PlayGoInfo *);

int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceSslInit(size_t);
int sceHttpInit(int, int, size_t);
int sceHttpCreateTemplate(int, const char *, int, int);
int sceHttpDeleteTemplate(int);
int sceHttpSetAutoRedirect(int, int);
typedef int (*SslCallback)(int, unsigned int, void *const *, int, void *);
int sceHttpsSetSslCallback(int, SslCallback, void *);
int sceHttpSetResolveTimeOut(int, uint32_t);
int sceHttpSetConnectTimeOut(int, uint32_t);
int sceHttpSetSendTimeOut(int, uint32_t);
int sceHttpSetRecvTimeOut(int, uint32_t);
int sceHttpCreateConnectionWithURL(int, const char *, int);
int sceHttpDeleteConnection(int);
int sceHttpCreateRequestWithURL(int, int, const char *, uint64_t);
int sceHttpDeleteRequest(int);
int sceHttpAddRequestHeader(int, const char *, const char *, int);
int sceHttpSendRequest(int, const void *, size_t);
int sceHttpGetStatusCode(int, int *);
int sceHttpGetLastErrno(int, int *);
int sceHttpGetResponseContentLength(int, int *, size_t *);
int sceHttpGetAllResponseHeaders(int, char **, size_t *);
int sceHttpReadData(int, void *, uint32_t);

_Static_assert(sizeof(PackageInfo) == 56, "PackageInfo ABI");
_Static_assert(offsetof(MetaInfo, unknown_30) == 0x30, "MetaInfo unknown tail offset");
_Static_assert(offsetof(MetaInfo, reserved) == 0x38, "MetaInfo reserved offset");
_Static_assert(sizeof(MetaInfo) == 0x70, "MetaInfo defensive capacity");
_Static_assert(offsetof(PlayGoInfo, playgo_scenario_ids) == 0xf0,
               "PlayGoInfo scenario offset");
_Static_assert(offsetof(PlayGoInfo, content_ids) == 0x1b0, "PlayGoInfo content IDs offset");
_Static_assert(offsetof(PlayGoInfo, unknown) == 0xdb0, "PlayGoInfo trailing pad offset");
_Static_assert(sizeof(PlayGoInfo) == 0x2700, "PlayGoInfo ABI");

typedef struct {
    char remote_url[MAX_URL];
    char content_id[CONTENT_ID_SIZE];
} PackageRequest;

static int http_ctx = -1;
static int proxy_socket = -1;
static volatile int worker_count;
static atomic_bool running = true;

#if PS5_PROXY_DEBUG_BUILD
int sceKernelSendNotificationRequest(int, Notification *, size_t, int);

static void notify(const char *fmt, ...) {
    Notification req;
    va_list args;

    memset(&req, 0, sizeof req);
    va_start(args, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, args);
    va_end(args);

    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* klog_printf truncates each line at 512 bytes; clip URLs with %.400s. */
#define klog_printf(fmt, ...) klog_printf("[PS5-PROXY] " fmt "\n", ##__VA_ARGS__)
#define EVENT(fmt, ...) do { \
        klog_printf(fmt, ##__VA_ARGS__); \
        notify("PS5 Package Proxy: " fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define klog_printf(...) ((void)0)
#define EVENT(...) ((void)0)
#endif

static void socket_timeout(int fd) {
    const struct timeval timeout = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int listen_on(uint32_t address, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
#ifdef __FreeBSD__
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(address);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Rest mode tears down the network stack and leaves listeners permanently failing. */
static int accept_client(int *listener, uint32_t address, uint16_t port) {
    while (atomic_load(&running)) {
        if (*listener < 0) {
            sleep(1);
            *listener = listen_on(address, port);
            if (*listener < 0) continue;
            EVENT("listening again on port %u", (unsigned)port);
        }
        int fd = accept(*listener, NULL, NULL);
        if (fd >= 0 || !atomic_load(&running)) return fd;
        if (errno == EINTR) continue;
        EVENT("accept on port %u failed errno=%d, reopening", (unsigned)port, errno);
        close(*listener);
        *listener = -1;
    }
    return -1;
}

static bool send_all(int fd, const void *data, size_t size) {
    const char *bytes = data;
    while (size) {
        ssize_t sent = send(fd, bytes, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes += sent;
        size -= (size_t)sent;
    }
    return true;
}

static bool read_line(int fd, char *out, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char ch;
        ssize_t count = recv(fd, &ch, 1, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        if (ch == '\n') break;
        if (ch == 0 || (unsigned char)ch >= 127 || ((unsigned char)ch < 32 && ch != '\r'))
            return false;
        out[used++] = ch;
    }
    if (used && out[used - 1] == '\r') --used;
    out[used] = 0;
    return used > 0 && used + 1 < capacity;
}

static bool valid_url(const char *url) {
    const char *host = NULL;
    if (strncmp(url, "https://", 8) == 0) host = url + 8;
    if (strncmp(url, "http://", 7) == 0) host = url + 7;
    if (!host || !*host || *host == '/' || *host == '?' || *host == '#') return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p) {
        if (*p <= 32 || *p >= 127 || *p == '#') return false;
    }
    return true;
}

static bool valid_content_id(const char *id) {
    if (strlen(id) != 36 || id[6] != '-' || id[16] != '_' || id[19] != '-') return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 6 || i == 16 || i == 19) continue;
        if ((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9') ||
            (i >= 20 && id[i] == '_')) continue;
        return false;
    }
    return true;
}

static const char base64url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static bool encode_url(const char *url, char *out, size_t capacity) {
    size_t length = strlen(url), used = 0;
    if (!valid_url(url) || length >= MAX_URL || capacity <= (length * 4 + 2) / 3) return false;
    uint32_t bits = 0;
    unsigned count = 0;
    for (size_t i = 0; i < length; ++i) {
        bits = (bits << 8) | (unsigned char)url[i];
        count += 8;
        while (count >= 6) {
            count -= 6;
            out[used++] = base64url_alphabet[(bits >> count) & 63];
        }
    }
    if (count) out[used++] = base64url_alphabet[(bits << (6 - count)) & 63];
    out[used] = 0;
    return true;
}

static bool decode_url(const char *encoded, size_t length, char *out, size_t capacity) {
    if (!length || length > MAX_ENCODED_URL || length % 4 == 1) return false;
    uint32_t bits = 0;
    unsigned count = 0;
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        /* strchr would treat a NUL byte as the alphabet's terminator. */
        const char *value = encoded[i] ? strchr(base64url_alphabet, encoded[i]) : NULL;
        if (!value) return false;
        bits = (bits << 6) | (unsigned)(value - base64url_alphabet);
        count += 6;
        if (count >= 8) {
            count -= 8;
            unsigned char ch = (unsigned char)(bits >> count);
            if (used + 1 >= capacity || ch <= 32 || ch >= 127) return false;
            out[used++] = (char)ch;
        }
    }
    /* Require canonical encoding: unused bits must be zero. */
    if (count && (bits & ((1U << count) - 1))) return false;
    out[used] = 0;
    return valid_url(out);
}

static bool make_local_url(const PackageRequest *package, char *out, size_t capacity) {
    char encoded[MAX_ENCODED_URL + 1];
    unsigned char random_id[REQUEST_ID_LENGTH / 2];
    char request_id[REQUEST_ID_LENGTH + 1];
    if (!valid_content_id(package->content_id) ||
        !encode_url(package->remote_url, encoded, sizeof(encoded))) return false;
    arc4random_buf(random_id, sizeof(random_id));
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random_id); ++i) {
        request_id[i * 2] = hex[random_id[i] >> 4];
        request_id[i * 2 + 1] = hex[random_id[i] & 15];
    }
    request_id[REQUEST_ID_LENGTH] = 0;
    int length = snprintf(out, capacity, "%s/v1/%s/%s/%s/pkg",
                          LOCAL_ORIGIN, package->content_id, request_id, encoded);
    return length > 0 && (size_t)length < capacity;
}

static void send_error(int fd, int code, const char *reason) {
    char header[192];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                          code, reason);
    if (length > 0 && (size_t)length < sizeof(header)) send_all(fd, header, (size_t)length);
}

static bool header_value(const char *headers, size_t size, const char *name,
                         char *out, size_t capacity) {
    size_t name_len = strlen(name);
    const char *p = headers;
    const char *end = headers + size;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        const char *value_end = line_end;
        if (value_end > p && value_end[-1] == '\r') --value_end;
        if ((size_t)(value_end - p) > name_len && p[name_len] == ':' &&
            strncasecmp(p, name, name_len) == 0) {
            const char *value = p + name_len + 1;
            while (value < value_end && (*value == ' ' || *value == '\t')) ++value;
            size_t length = (size_t)(value_end - value);
            if (!length || length >= capacity) return false;
            memcpy(out, value, length);
            out[length] = 0;
            return true;
        }
        p = line_end == end ? end : line_end + 1;
    }
    return false;
}

static bool valid_package_path(const char *path) {
    if (strcmp(path, "/pkg") == 0) return true;
    if (strncmp(path, "/pkg/", 5) != 0) return false;
    const char *p = path + 5;
    if (*p < '0' || *p > '9') return false;
    do { ++p; } while (*p >= '0' && *p <= '9');
    return strcmp(p, ".pkg") == 0;
}

/* Decode a self-contained route. No prior install command or registry is needed. */
static bool decode_package_path(char *path, PackageRequest *package) {
    if (strncmp(path, "/v1/", 4) != 0) return false;
    char *id = path + 4;
    char *request_id = strchr(id, '/');
    if (!request_id) return false;
    *request_id++ = 0;
    if (!valid_content_id(id)) return false;
    char *encoded = strchr(request_id, '/');
    if (!encoded || (size_t)(encoded - request_id) != REQUEST_ID_LENGTH ||
        strspn(request_id, "0123456789abcdef") != REQUEST_ID_LENGTH) return false;
    ++encoded;
    char *suffix = strchr(encoded, '/');
    if (!suffix || !valid_package_path(suffix) ||
        !decode_url(encoded, (size_t)(suffix - encoded), package->remote_url,
                    sizeof(package->remote_url))) return false;
    memcpy(package->content_id, id, strlen(id) + 1);
    return true;
}

/* Returns NULL on success, otherwise the rejection reason. */
static const char *parse_request(int fd, bool *head, char *range, size_t range_size,
                                 PackageRequest *package) {
    char request[MAX_REQUEST];
    size_t used = 0;
    while (used + 1 < sizeof(request)) {
        ssize_t n = recv(fd, request + used, sizeof(request) - 1 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return "connection closed or timed out before headers";
        if (memchr(request + used, 0, (size_t)n)) return "NUL byte in request";
        used += (size_t)n;
        request[used] = 0;
        if (strstr(request, "\r\n\r\n")) break;
    }
    char *line_end = strstr(request, "\r\n");
    if (!line_end || !strstr(request, "\r\n\r\n")) return "headers too large";
    *line_end = 0;
    klog_printf("local request: %.400s", request);
    char *path = strchr(request, ' ');
    if (!path) return "malformed request line";
    *path++ = 0;
    char *version = strchr(path, ' ');
    if (!version) return "malformed request line";
    *version++ = 0;
    if (strcmp(request, "GET") == 0) *head = false;
    else if (strcmp(request, "HEAD") == 0) *head = true;
    else return "unsupported method";
    /* PlayGo appends ?product=...&serverIpAddr=...&r=... to the package URL. */
    char *query = strchr(path, '?');
    if (query) *query = 0;
    if (!decode_package_path(path, package)) return "invalid package path";
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)
        return "unsupported HTTP version";
    const char *headers = line_end + 2;
    size_t header_size = used - (size_t)(headers - request);
    range[0] = 0;
    bool has_range = header_value(headers, header_size, "Range", range, range_size);
    if (!has_range) {
        const char *p = headers, *end = headers + header_size;
        while (p < end) {
            const char *next = memchr(p, '\n', (size_t)(end - p));
            if (!next) next = end;
            if (next - p >= 6 && strncasecmp(p, "Range:", 6) == 0) return "unreadable Range header";
            p = next == end ? end : next + 1;
        }
    } else {
        klog_printf("local request range: %s", range);
        if (strncmp(range, "bytes=", 6) != 0 || strchr(range, ',') ||
            !strchr(range + 6, '-')) return "unsupported Range";
        int hyphens = 0, digits = 0;
        for (const char *p = range + 6; *p; ++p) {
            if (*p == '-') ++hyphens;
            else if (*p >= '0' && *p <= '9') ++digits;
            else return "unsupported Range";
        }
        if (hyphens != 1 || digits == 0) return "unsupported Range";
    }
    return NULL;
}

static bool range_number(const char **cursor, uint64_t *value) {
    const char *p = *cursor;
    if (*p < '0' || *p > '9') return false;
    char *end;
    errno = 0;
    unsigned long long number = strtoull(p, &end, 10);
    if (errno) return false;
    *cursor = end;
    *value = number;
    return true;
}

static bool range_matches(const char *request, const char *response,
                          int length_type, size_t content_length) {
    if (strncmp(request, "bytes=", 6) != 0 || strncasecmp(response, "bytes ", 6) != 0)
        return false;
    const char *p = response + 6;
    uint64_t first, last, total = 0;
    if (!range_number(&p, &first) || *p != '-') return false;
    ++p;
    if (!range_number(&p, &last) || *p != '/' || last < first || last - first == UINT64_MAX)
        return false;
    ++p;
    bool total_known = *p != '*';
    if (total_known) {
        if (!range_number(&p, &total) || total <= last) return false;
    } else ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p) return false;
    uint64_t count = last - first + 1;
    if (length_type == 0 && count != content_length) return false;
    p = request + 6;
    if (*p == '-') {
        uint64_t suffix;
        ++p;
        if (!range_number(&p, &suffix) || *p || !suffix || !total_known) return false;
        return first == (total > suffix ? total - suffix : 0) && last == total - 1;
    }
    uint64_t requested_first;
    if (!range_number(&p, &requested_first) || *p != '-' || requested_first != first) return false;
    ++p;
    if (*p) {
        uint64_t requested_last;
        if (!range_number(&p, &requested_last) || *p || last > requested_last) return false;
    }
    return true;
}
static int start_http(void) {
    int result = sceNetInit();
    if (result < 0) return result;
    int pool = sceNetPoolCreate("PS5PackageProxy", 1024 * 1024, 0);
    if (pool < 0) return pool;
    int ssl = sceSslInit(512 * 1024);
    if (ssl < 0) return ssl;
    http_ctx = sceHttpInit(pool, ssl, 1024 * 1024);
    return http_ctx;
}

/* libSceSsl accepts the chain when this returns >= 0, even on verify errors
 * such as the unknown Cloudflare CA (0x8095F00C). */
static int accept_any_certificate(int ssl_ctx, unsigned int verify_error,
                                  void *const *certs, int cert_count, void *user) {
    (void)ssl_ctx; (void)verify_error; (void)certs; (void)cert_count; (void)user;
    return 0;
}

static void serve_package(int fd) {
    bool head;
    char range[128];
    PackageRequest package;
    const char *reason = parse_request(fd, &head, range, sizeof(range), &package);
    if (reason) {
        EVENT("rejected local request: %s", reason);
        send_error(fd, 400, "Bad Request");
        return;
    }
    int tmpl = -1, conn = -1, req = -1, result, status = 0;
    char *raw_headers = NULL;
    size_t raw_size = 0, content_length = 0;
    int length_type = 1;
    char content_range[128] = {0};
    uint64_t sent_bytes = 0;
    bool chunked = false;
    const char *step = "create template";

    result = tmpl = sceHttpCreateTemplate(http_ctx, "PS5-Package-Proxy/1.0", 2, 1);
    if (tmpl < 0) goto failed;
    step = "enable redirects";
    if ((result = sceHttpSetAutoRedirect(tmpl, 1)) < 0) goto failed;
    /* sceHttpsDisableOption gates the verify flags behind a registry check. */
    step = "disable TLS verification";
    if ((result = sceHttpsSetSslCallback(tmpl, accept_any_certificate, NULL)) < 0) goto failed;
    sceHttpSetResolveTimeOut(tmpl, 30000000);
    sceHttpSetConnectTimeOut(tmpl, 30000000);
    sceHttpSetSendTimeOut(tmpl, 30000000);
    sceHttpSetRecvTimeOut(tmpl, 30000000);
    step = "create connection";
    result = conn = sceHttpCreateConnectionWithURL(tmpl, package.remote_url, 0);
    if (conn < 0) goto failed;
    /* A GET also supplies headers for local HEAD when the origin rejects HEAD. */
    step = "create request";
    result = req = sceHttpCreateRequestWithURL(conn, 0, package.remote_url, 0);
    if (req < 0) goto failed;
    step = "add headers";
    if ((result = sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", 1)) < 0) goto failed;
    if (range[0] && (result = sceHttpAddRequestHeader(req, "Range", range, 1)) < 0) goto failed;
    step = "send request";
    result = sceHttpSendRequest(req, NULL, 0);
    if (result < 0) {
        int net_errno = 0;
        sceHttpGetLastErrno(req, &net_errno);
        klog_printf("upstream net_errno=%d", net_errno);
        goto failed;
    }
    step = "read status";
    if ((result = sceHttpGetStatusCode(req, &status)) < 0) goto failed;
    if (range[0] && status == 200) {
        EVENT("upstream ignored Range %s", range);
        goto bad_gateway;
    }
    if (status != 200 && status != 206 && status != 416) {
        EVENT("upstream returned HTTP %d", status);
        goto bad_gateway;
    }
    sceHttpGetResponseContentLength(req, &length_type, &content_length);
    if (sceHttpGetAllResponseHeaders(req, &raw_headers, &raw_size) >= 0 && raw_headers) {
        header_value(raw_headers, raw_size, "Content-Range", content_range, sizeof(content_range));
    }
    if (status == 206 && (!range[0] || !content_range[0] ||
                          !range_matches(range, content_range, length_type, content_length))) {
        EVENT("upstream Content-Range '%s' does not match Range '%s'", content_range, range);
        goto bad_gateway;
    }
    if (status == 416) content_length = 0;
    chunked = length_type != 0 && status != 416;
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Accept-Ranges: bytes\r\n"
        "Connection: close\r\n",
        status, status == 206 ? "Partial Content" : status == 416 ? "Range Not Satisfiable" : "OK");
    if (header_len < 0 || (size_t)header_len >= sizeof(header) ||
        !send_all(fd, header, (size_t)header_len)) goto client_gone;
    if (content_range[0]) {
        header_len = snprintf(header, sizeof(header), "Content-Range: %s\r\n", content_range);
        if (header_len < 0 || (size_t)header_len >= sizeof(header) ||
            !send_all(fd, header, (size_t)header_len)) goto client_gone;
    }
    if (chunked) header_len = snprintf(header, sizeof(header), "Transfer-Encoding: chunked\r\n\r\n");
    else header_len = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", content_length);
    if (header_len < 0 || (size_t)header_len >= sizeof(header) ||
        !send_all(fd, header, (size_t)header_len)) goto client_gone;
    klog_printf("%s %s -> %d length=%zu chunked=%d content_range=%s", head ? "HEAD" : "GET",
        range[0] ? range : "full", status, content_length, chunked,
        content_range[0] ? content_range : "none");
    if (head || status == 416) goto done;

    char buffer[CHUNK_SIZE];
    for (;;) {
        int got = sceHttpReadData(req, buffer, sizeof(buffer));
        if (got == 0) break;
        if (got < 0) {
            EVENT("upstream read failed 0x%08X after %llu bytes", (unsigned)got,
                  (unsigned long long)sent_bytes);
            goto done;
        }
        if (chunked) {
            char prefix[24];
            int prefix_len = snprintf(prefix, sizeof(prefix), "%X\r\n", (unsigned)got);
            if (prefix_len < 0 || !send_all(fd, prefix, (size_t)prefix_len) ||
                !send_all(fd, buffer, (size_t)got) || !send_all(fd, "\r\n", 2)) goto client_gone;
        } else if (!send_all(fd, buffer, (size_t)got)) goto client_gone;
        sent_bytes += (uint64_t)got;
    }
    if (chunked) send_all(fd, "0\r\n\r\n", 5);
    else if (sent_bytes != content_length) {
        EVENT("upstream ended early: %llu of %zu bytes", (unsigned long long)sent_bytes,
              content_length);
        goto done;
    }
    klog_printf("stream complete: %llu bytes", (unsigned long long)sent_bytes);
    goto done;

client_gone:
    klog_printf("local client closed after %llu bytes errno=%d", (unsigned long long)sent_bytes, errno);
    goto done;
failed:
    EVENT("upstream %s failed 0x%08X", step, (unsigned)result);
    (void)step;
bad_gateway:
    send_error(fd, 502, "Bad Gateway");
done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
}

static void *proxy_client(void *argument) {
    int fd = (int)(intptr_t)argument;
    socket_timeout(fd);
    serve_package(fd);
    close(fd);
    __sync_sub_and_fetch(&worker_count, 1);
    return NULL;
}

static void *proxy_server(void *unused) {
    (void)unused;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (;;) {
        int fd = accept_client(&proxy_socket, 0x7f000001, PROXY_PORT);
        if (fd < 0) break;
        if (__sync_add_and_fetch(&worker_count, 1) > MAX_WORKERS) {
            __sync_sub_and_fetch(&worker_count, 1);
            EVENT("more than %d local downloads, rejecting one", MAX_WORKERS);
            send_error(fd, 503, "Service Unavailable");
            close(fd);
            continue;
        }
        pthread_t thread;
        int error = pthread_create(&thread, &attr, proxy_client, (void *)(intptr_t)fd);
        if (error != 0) {
            __sync_sub_and_fetch(&worker_count, 1);
            EVENT("worker thread creation failed error=%d", error);
            send_error(fd, 503, "Service Unavailable");
            close(fd);
        }
    }
    pthread_attr_destroy(&attr);
    return NULL;
}

static int queue_install(const PackageRequest *source, const char *task_name) {
    char local_url[MAX_LOCAL_URL];
    if (!make_local_url(source, local_url, sizeof(local_url))) return -1;
    MetaInfo meta;
    memset(&meta, 0, sizeof(meta));
    meta.uri = local_url;
    meta.ex_uri = "";
    meta.playgo_scenario_id = "";
    meta.content_id = source->content_id;
    meta.content_name = task_name;
    meta.icon_url = "";
    PackageInfo package = {0};
    PlayGoInfo playgo = {0};
    int result;
    for (unsigned attempt = 1; ; ++attempt) {
        memset(&package, 0, sizeof(package));
        memset(&playgo, 0, sizeof(playgo));
        result = sceAppInstUtilInstallByPackage(&meta, &package, &playgo);
        /* Do not retry accepted calls, other errors, or errors that returned
         * package information: those may have made further install progress. */
        if (result != INSTALL_INVALID_SLOT_ERROR || package.content_id[0] ||
            package.content_type || package.content_platform ||
            attempt == INSTALL_MAX_ATTEMPTS || !atomic_load(&running)) break;
        EVENT("invalid install slot: 0x%08X; retrying in %u s (%u/%u)",
              (unsigned)result, INSTALL_RETRY_SECONDS, attempt + 1, INSTALL_MAX_ATTEMPTS);
        unsigned remaining = INSTALL_RETRY_SECONDS;
        while (remaining && atomic_load(&running)) remaining = sleep(remaining);
        if (!atomic_load(&running)) break;
    }
    package.content_id[sizeof(package.content_id) - 1] = 0;
    EVENT("install request %s: 0x%08X content_id=%s", result == 0 ? "queued" : "failed",
          (unsigned)result, package.content_id);
    return result;
}

static void handle_control(int fd) {
    char input[MAX_URL + CONTENT_ID_SIZE + MAX_TASK_NAME + 10];
    if (!read_line(fd, input, sizeof(input))) {
        EVENT("control: empty or oversized command");
        send_all(fd, "-1\n", 3);
        return;
    }
    if (strcmp(input, "stop") == 0) {
        EVENT("stop requested with %d active downloads", worker_count);
        send_all(fd, "0\n", 2);
        atomic_store(&running, false);
        return;
    }
    EVENT("control: received %.400s", input);
    if (strncmp(input, "install ", 8) != 0) {
        send_all(fd, "-1\n", 3);
        return;
    }
    char *content_id = input + 8;
    char *url = strchr(content_id, ' ');
    if (!url) {
        send_all(fd, "-1\n", 3);
        return;
    }
    *url++ = 0;
    /* The optional name occupies the rest of the line, so spaces need no quoting. */
    char *task_name = strchr(url, ' ');
    if (task_name) {
        *task_name++ = 0;
        while (*task_name == ' ') ++task_name;
        size_t length = strlen(task_name);
        while (length && task_name[length - 1] == ' ') task_name[--length] = 0;
        if (length >= MAX_TASK_NAME || strchr(task_name, '\r')) {
            EVENT("install rejected: invalid or oversized task name");
            send_all(fd, "-1\n", 3);
            return;
        }
        if (!length) task_name = NULL;
    }
    if (!valid_content_id(content_id) || strlen(url) >= MAX_URL || !valid_url(url)) {
        EVENT("install rejected: invalid content ID or URL");
        send_all(fd, "-1\n", 3);
        return;
    }
    PackageRequest source;
    memcpy(source.content_id, content_id, strlen(content_id) + 1);
    memcpy(source.remote_url, url, strlen(url) + 1);
    /* AppInstUtil retains the self-contained URI. No submission state survives. */
    int result = queue_install(&source, task_name ? task_name : source.remote_url);
    char reply[32];
    int length = snprintf(reply, sizeof(reply), "%d\n", result);
    if (length > 0) send_all(fd, reply, (size_t)length);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    EVENT("starting");
    int result = start_http();
    if (result < 0) {
        EVENT("HTTP initialization failed 0x%08X", (unsigned)result);
        return 1;
    }
    result = sceAppInstUtilInitialize();
    if (result != 0) EVENT("AppInstUtil initialization returned 0x%08X, continuing", (unsigned)result);
    proxy_socket = listen_on(0x7f000001, PROXY_PORT);
    int control_socket = listen_on(0, CONTROL_PORT);
    if (proxy_socket < 0 || control_socket < 0) {
        EVENT("cannot listen: port %d fd=%d, port %d fd=%d, errno=%d",
              CONTROL_PORT, control_socket, PROXY_PORT, proxy_socket, errno);
        return 1;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, proxy_server, NULL) != 0) {
        EVENT("proxy thread creation failed");
        return 1;
    }
    pthread_detach(thread);
    EVENT("ready on port %d, proxy 127.0.0.1:%d", CONTROL_PORT, PROXY_PORT);
    for (;;) {
        int fd = accept_client(&control_socket, 0, CONTROL_PORT);
        if (fd < 0) break;
        socket_timeout(fd);
        handle_control(fd);
        close(fd);
    }
    /* Returning ends the process, which also ends any worker still streaming. */
    close(control_socket);
    if (result == 0) sceAppInstUtilTerminate();
    return 0;
}
