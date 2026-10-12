// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define main payload_main
#define sleep test_sleep
#include "main.c"
#undef main
#undef sleep

#include <assert.h>

#if PS5_PROXY_DEBUG_BUILD
int (klog_printf)(const char *fmt, ...) { (void)fmt; return 0; }
int sceKernelSendNotificationRequest(int device, Notification *req, size_t size, int blocking) {
    (void)device; (void)req; (void)size; (void)blocking; return 0;
}
#endif

#define TEST_ID "XX0000-CUSA00001_00-ABCDEFGHIJKLMNOP"
#define OTHER_ID "YY0001-PPSA00002_00-QRSTUVWXYZ012345"
#define TEST_URL "https://example.invalid/pkg?token=abc&name=Package%20Update"
#define INSTALL_COMMAND "install " TEST_ID " " TEST_URL

static int install_calls;
static int install_result;
static int transient_failures;
static int return_package_info;
static unsigned retry_delays;
static const char *expected_url = TEST_URL;
static const char *expected_id = TEST_ID;
static const char *expected_name;
static char submitted_url[MAX_LOCAL_URL];
static char previous_url[MAX_LOCAL_URL];
static bool within_submission;

unsigned test_sleep(unsigned seconds) {
    assert(seconds == INSTALL_RETRY_SECONDS);
    ++retry_delays;
    return 0;
}

int sceAppInstUtilInstallByPackage(MetaInfo *meta, PackageInfo *package, PlayGoInfo *playgo) {
    /* Check the zeroed input ranges from the PS5Upload installer header. */
    const unsigned char zero_meta_tail[0x40] = {0};
    const unsigned char zero_playgo[0x2700] = {0};
    assert(memcmp((const unsigned char *)meta + 0x30, zero_meta_tail,
                  sizeof(zero_meta_tail)) == 0);
    assert(memcmp(playgo, zero_playgo, sizeof(zero_playgo)) == 0);
    assert(strcmp(meta->content_id, expected_id) == 0);
    assert(strcmp(meta->content_name, expected_name ? expected_name : expected_url) == 0);
    assert(strncmp(meta->uri, LOCAL_ORIGIN "/v1/", strlen(LOCAL_ORIGIN "/v1/")) == 0);
    assert(strchr(meta->uri, '?') == NULL);
    char path[MAX_LOCAL_URL];
    strcpy(path, meta->uri + strlen(LOCAL_ORIGIN));
    PackageRequest decoded;
    assert(decode_package_path(path, &decoded));
    assert(strcmp(decoded.remote_url, expected_url) == 0);
    assert(strcmp(decoded.content_id, expected_id) == 0);
    if (within_submission) assert(strcmp(submitted_url, meta->uri) == 0);
    else {
        assert(strcmp(previous_url, meta->uri) != 0);
        strcpy(submitted_url, meta->uri);
        within_submission = true;
    }
    ++install_calls;
    /* Rejected-call output must be cleared before any bounded retry. */
    assert(package->content_id[0] == 0);
    assert(package->content_type == 0 && package->content_platform == 0);
    if (return_package_info == 1) strcpy(package->content_id, expected_id);
    if (return_package_info == 2) package->content_type = 1;
    if (transient_failures > 0) {
        --transient_failures;
        return INSTALL_INVALID_SLOT_ERROR;
    }
    return install_result;
}

static void command_bytes(const char *input, size_t length, const char *expected_reply) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    strcpy(previous_url, submitted_url);
    within_submission = false;
    assert(send_all(sockets[0], input, length));
    assert(send_all(sockets[0], "\n", 1));
    handle_control(sockets[1]);
    close(sockets[1]);
    char reply[64];
    ssize_t n = recv(sockets[0], reply, sizeof(reply) - 1, 0);
    assert(n > 0);
    reply[n] = 0;
    assert(strcmp(reply, expected_reply) == 0);
    close(sockets[0]);
}

static void command(const char *input, const char *expected_reply) {
    command_bytes(input, strlen(input), expected_reply);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *invalid[] = {
        TEST_URL, "install 1.02 " TEST_URL, "retry " TEST_ID " " TEST_URL,
        "install  " TEST_URL, "install " TEST_ID, "install ../id " TEST_URL,
        "install " TEST_ID " file:///invalid.pkg",
        "install " TEST_ID " https://example.invalid/pkg#fragment",
        "install " TEST_ID " https://example.invalid/\rhidden",
        "install XX0000-CUSA00001_00-ABCDEFGHIJKLMNO " TEST_URL
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) command(invalid[i], "-1\n");
    const char nul_command[] = INSTALL_COMMAND "\0ignored";
    command_bytes(nul_command, sizeof(nul_command) - 1, "-1\n");
    assert(install_calls == 0);

    install_result = -99;
    command(INSTALL_COMMAND, "-99\n");
    assert(install_calls == 1);
    install_result = 0;
    command(INSTALL_COMMAND, "0\n");
    command(INSTALL_COMMAND, "0\n");
    assert(install_calls == 3); /* Every submission is fresh, even after acceptance. */
    char first_url[MAX_LOCAL_URL];
    strcpy(first_url, submitted_url);
    expected_id = OTHER_ID;
    expected_url = "http://example.invalid/another.pkg?sig=A%2FB%3D&x=1";
    command("install " OTHER_ID " http://example.invalid/another.pkg?sig=A%2FB%3D&x=1", "0\n");
    char old_path[MAX_LOCAL_URL];
    strcpy(old_path, first_url + strlen(LOCAL_ORIGIN));
    PackageRequest old_package;
    assert(decode_package_path(old_path, &old_package));
    assert(strcmp(old_package.content_id, TEST_ID) == 0 && strcmp(old_package.remote_url, TEST_URL) == 0);

    expected_id = TEST_ID;
    expected_url = TEST_URL;
    transient_failures = 1;
    command(INSTALL_COMMAND, "0\n");
    assert(install_calls == 6 && retry_delays == 1);
    transient_failures = INSTALL_MAX_ATTEMPTS;
    char rejected_reply[32];
    snprintf(rejected_reply, sizeof(rejected_reply), "%d\n", INSTALL_INVALID_SLOT_ERROR);
    command(INSTALL_COMMAND, rejected_reply);
    assert(install_calls == 9 && retry_delays == 3);
    command(INSTALL_COMMAND, "0\n");
    assert(install_calls == 10 && retry_delays == 3);
    transient_failures = 1;
    install_result = -99;
    command(INSTALL_COMMAND, "-99\n");
    assert(install_calls == 12 && retry_delays == 4);
    install_result = INSTALL_INVALID_SLOT_ERROR;
    return_package_info = 1;
    command(INSTALL_COMMAND, rejected_reply);
    return_package_info = 2;
    command(INSTALL_COMMAND, rejected_reply);
    assert(install_calls == 14 && retry_delays == 4);
    return_package_info = 0;
    install_result = 0;

    /* The maximum upstream URL must fit both the command and encoded local URI. */
    char longest[MAX_URL];
    strcpy(longest, "https://example.invalid/");
    size_t prefix_length = strlen(longest);
    memset(longest + prefix_length, 'a', sizeof(longest) - prefix_length - 1);
    longest[sizeof(longest) - 1] = 0;
    char long_command[MAX_URL + CONTENT_ID_SIZE + 9];
    snprintf(long_command, sizeof(long_command), "install %s %s", TEST_ID, longest);
    expected_url = longest;
    command(long_command, "0\n");
    assert(install_calls == 15 && strlen(submitted_url) > MAX_URL);
    long_command[strlen(long_command)] = 'a';
    long_command[8 + strlen(TEST_ID) + 1 + MAX_URL] = 0;
    command(long_command, "-1\n");
    assert(install_calls == 15);

    /* Losing the client does not stop the bounded recovery already in flight. */
    int sockets[2];
    expected_url = TEST_URL;
    transient_failures = 1;
    within_submission = false;
    strcpy(previous_url, submitted_url);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(send_all(sockets[0], INSTALL_COMMAND "\n", strlen(INSTALL_COMMAND "\n")));
    close(sockets[0]);
    handle_control(sockets[1]);
    close(sockets[1]);
    assert(install_calls == 17 && retry_delays == 5);

    /* Names are submission-only metadata; decoded routes retain the original URL. */
    expected_name = "Package Installer Update";
    command(INSTALL_COMMAND " Package Installer Update", "0\n");
    command(INSTALL_COMMAND "   Package Installer Update   \r", "0\n");
    transient_failures = 1;
    command(INSTALL_COMMAND " Package Installer Update", "0\n");
    assert(install_calls == 21 && retry_delays == 6);
    expected_name = NULL;
    command(INSTALL_COMMAND, "0\n");
    command(INSTALL_COMMAND "   ", "0\n");
    assert(install_calls == 23);
    command(INSTALL_COMMAND " bad\tname", "-1\n");
    command(INSTALL_COMMAND " bad\rname", "-1\n");
    const char nul_name[] = INSTALL_COMMAND " name\0suffix";
    command_bytes(nul_name, sizeof(nul_name) - 1, "-1\n");
    assert(install_calls == 23);

    char longest_name[MAX_TASK_NAME];
    memset(longest_name, 'n', sizeof(longest_name) - 1);
    longest_name[sizeof(longest_name) - 1] = 0;
    char named_command[MAX_URL + CONTENT_ID_SIZE + MAX_TASK_NAME + 10];
    expected_url = longest;
    expected_name = longest_name;
    snprintf(named_command, sizeof(named_command), "install %s %s %s", TEST_ID, longest, longest_name);
    command(named_command, "0\n");
    assert(install_calls == 24);
    size_t named_length = strlen(named_command);
    named_command[named_length] = 'n';
    named_command[named_length + 1] = 0;
    command(named_command, "-1\n");
    assert(install_calls == 24);
    command("stop", "0\n");
    assert(!atomic_load(&running));
    puts("PASS: zeroed installer ABI inputs, optional task names and URL fallback, dynamic IDs, fresh stateless submissions, delayed routes, bounded recovery, limits, stop");
    return 0;
}
