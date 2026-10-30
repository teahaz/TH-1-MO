/*
 * The link and the remote backend: bus-replay link --dummy (test_bin()) on a socket in the
 * test's directory, and capture sessions through it. A capture started through the link
 * gives clean records; a second client is refused; a client that goes away leaves the
 * capture running, and the next one joins it at a heartbeat; a client too slow for the
 * link's queue gets an overflow event and a clean stream after it; the link records and
 * ends the recording when it exits.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "capture/capture.h"
#include "capture/remote.h"
#include "capture/thcfile.h"
#include "common/record.h"
#include "common/status.h"
#include "test/test.h"


// What a session handed over.
struct tally {
    uint64_t records;
    uint64_t heartbeats;
    uint64_t overflows;
};


static pid_t link_start(const char *sock, const char *queue, const char *record);
static void link_end(pid_t pid);
static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static void on_event(void *ctx, uint16_t code, uint16_t channel, const uint8_t *data,
                     size_t len);
static void pull_for(struct capture *cap, unsigned int ms);
static void sleep_ms(unsigned int ms);
static void test_start(const char *sock);
static void test_join(const char *sock);
static void test_slow(const char *dir);
static void test_record(const char *dir);


int main(void)
{
    const char *dir = test_dir();
    char sock[600];
    pid_t pid = 0;

    snprintf(sock, sizeof(sock), "%s/link.sock", dir);
    pid = link_start(sock, NULL, NULL);
    test_start(sock);
    test_join(sock);
    link_end(pid);
    CHECK(access(sock, F_OK) < 0);

    test_slow(dir);
    test_record(dir);

    return test_result(__FILE__);
}


// bus-replay link --dummy on sock, once its socket is there.
static pid_t link_start(const char *sock, const char *queue, const char *record)
{
    pid_t pid = fork();

    if (pid == 0) {
        execl(test_bin(), "bus-replay", "link", "--dummy", "--socket", sock,
              queue ? "--queue" : NULL, queue, record ? "--record" : NULL, record,
              (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 400 && access(sock, F_OK) < 0; i++) {
        sleep_ms(5);
    }
    CHECK(access(sock, F_OK) == 0);

    return pid;
}


static void link_end(pid_t pid)
{
    int status = 0;

    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}


static void on_record(void *ctx, const struct record *r)
{
    struct tally *t = ctx;

    (void)r;
    t->records++;
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct tally *t = ctx;

    (void)hb;
    t->heartbeats++;
}


static void on_event(void *ctx, uint16_t code, uint16_t channel, const uint8_t *data,
                     size_t len)
{
    struct tally *t = ctx;

    (void)channel;
    (void)data;
    (void)len;
    if (code == THC_EV_OVERFLOW) {
        t->overflows++;
    }
}


static void pull_for(struct capture *cap, unsigned int ms)
{
    struct timespec a;
    struct timespec b;

    clock_gettime(CLOCK_MONOTONIC, &a);
    do {
        CHECK(capture_pull(cap, 10) >= 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
    } while ((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000 < (long)ms);
}


static void sleep_ms(unsigned int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}


// A capture through the link, as through a device; a second client is turned away.
static void test_start(const char *sock)
{
    struct tally t = {0};
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .event = on_event, .ctx = &t};
    struct remote_options o = {.path = sock};
    struct remote_status rs;
    struct capture *cap = NULL;
    struct capture_status st;
    void *other = NULL;
    char error[256];

    CHECK(capture_open_remote(&cap, &o, &h) == 0);
    if (!cap) {
        return;
    }
    CHECK(!remote_running(capture_source(cap)));
    CHECK(remote_open(&other, &o) == -EBUSY);
    CHECK(capture_start(cap, NULL, error, sizeof(error)) == 0);
    CHECK(remote_running(capture_source(cap)));
    pull_for(cap, 1000);
    capture_status(cap, &st);
    CHECK(t.records > 5000 && t.heartbeats > 400);
    CHECK(st.total.crc_errors == 0 && st.total.gap_errors == 0 && st.total.resyncs == 0);
    CHECK(remote_status(capture_source(cap), &rs) == 0 && rs.running && !rs.recording);
    CHECK(remote_record(capture_source(cap), "x.thcapture") == -EBUSY);
    CHECK(capture_stop(cap, THC_REASON_USER) == 0);
    CHECK(remote_status(capture_source(cap), &rs) == 0 && !rs.running);
    capture_close(cap);
}


/*
 * A client starts a capture and goes away without stopping it; the next one finds it
 * running and joins it, with a clean stream from its first heartbeat on.
 */
static void test_join(const char *sock)
{
    struct tally t = {0};
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .event = on_event, .ctx = &t};
    struct remote_options o = {.path = sock};
    struct capture_attach a;
    struct capture *cap = NULL;
    struct capture_status st;
    char error[256];
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        if (capture_open_remote(&cap, &o, NULL) < 0 ||
            capture_start(cap, NULL, error, sizeof(error)) < 0) {
            _exit(1);
        }
        pull_for(cap, 300);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    for (int i = 0; i < 100 && !cap; i++) {
        if (capture_open_remote(&cap, &o, &h) == -EBUSY) {
            sleep_ms(10);
        }
    }
    CHECK(cap != NULL);
    if (!cap) {
        return;
    }
    CHECK(remote_running(capture_source(cap)));
    CHECK(remote_attach(capture_source(cap), &a) == 0);
    CHECK(a.first_grid > 0 && a.config_count >= 2 && a.hello_len > 0);
    CHECK(capture_attach(cap, &a, error, sizeof(error)) == 0);
    pull_for(cap, 1000);
    capture_status(cap, &st);
    CHECK(t.records > 5000 && t.heartbeats > 400);
    CHECK(st.total.crc_errors == 0 && st.total.gap_errors == 0 && st.total.resyncs == 0);
    CHECK(st.now > a.first_grid * capture_info(cap)->c);
    CHECK(capture_stop(cap, THC_REASON_USER) == 0);
    capture_close(cap);
}


/*
 * A client that does not read past the link's queue (4 KiB here) and the socket's buffers:
 * the link drops for it, and says so with an overflow event, after which the stream parses
 * cleanly again.
 */
static void test_slow(const char *dir)
{
    struct tally t = {0};
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .event = on_event, .ctx = &t};
    char sock[600];
    struct remote_options o = {.path = sock};
    struct remote_status rs;
    struct capture *cap = NULL;
    struct capture_status st;
    char error[256];
    pid_t pid = 0;

    snprintf(sock, sizeof(sock), "%s/slow.sock", dir);
    pid = link_start(sock, "4096", NULL);
    CHECK(capture_open_remote(&cap, &o, &h) == 0);
    if (cap) {
        CHECK(capture_start(cap, NULL, error, sizeof(error)) == 0);
        sleep_ms(7000);
        pull_for(cap, 1500);
        capture_status(cap, &st);
        CHECK(t.overflows > 0 && capture_overflow(cap) > 0);
        CHECK(remote_status(capture_source(cap), &rs) == 0 && rs.dropped_bytes > 0);
        CHECK(st.total.crc_errors == 0);
        CHECK(capture_stop(cap, THC_REASON_USER) == 0);
        capture_close(cap);
    }
    link_end(pid);
}


/*
 * The link records the capture, which goes on after its client has gone, and closes the
 * file properly when it exits (reason: signalled).
 */
static void test_record(const char *dir)
{
    char sock[600];
    char path[600];
    struct remote_options o = {.path = sock};
    struct capture *cap = NULL;
    struct capture *file = NULL;
    const struct thc_trailer *tr = NULL;
    char error[256];
    pid_t pid = 0;
    pid_t child = 0;
    int status = 0;

    snprintf(sock, sizeof(sock), "%s/rec.sock", dir);
    snprintf(path, sizeof(path), "%s/linked.thcapture", dir);
    pid = link_start(sock, NULL, NULL);
    child = fork();
    if (child == 0) {
        struct remote_status rs;

        if (capture_open_remote(&cap, &o, NULL) < 0 ||
            remote_record(capture_source(cap), path) < 0 ||
            capture_start(cap, NULL, error, sizeof(error)) < 0) {
            _exit(1);
        }
        pull_for(cap, 500);
        if (remote_status(capture_source(cap), &rs) < 0 || !rs.recording ||
            strcmp(rs.path, path)) {
            _exit(2);
        }
        _exit(0);
    }
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    sleep_ms(500);
    link_end(pid);

    CHECK(capture_open_file(&file, path, NULL) == 0);
    if (file) {
        while (capture_pull(file, 0) > 0) {
        }
        CHECK(thc_reader_state(capture_reader(file)) == THC_ENDED);
        tr = thc_reader_trailer(capture_reader(file));
        CHECK(tr && tr->reason == THC_REASON_SIGNAL);
        CHECK(capture_channel_records(file)[0] > 1000);
        capture_close(file);
    }
}
