/*
 * bus-replay capture: one capture from the device (or the dummy), recorded to a capture
 * file, with its status on stderr every second. Ctrl-C stops it cleanly. With --decode it
 * also decodes as it goes (decoding.c, the same options and output as decode), writing the
 * items to stdout and only a final status.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bits/time.h>

#include "capture/board.h"
#include "capture/capture.h"
#include "capture/control.h"
#include "capture/dummy.h"
#include "capture/recorder.h"
#include "capture/thcfile.h"
#include "common/capture_info.h"
#include "common/status.h"
#include "modes/decoding.h"
#include "modes/modes.h"


#define POLL_MS 100
#define STATUS_MS 1000
// A loss still open at the end is reported once it is over: wait for its report, so the
// mismatches it explains are not counted as errors.
#define DRAIN_MS 500

#define EXIT_ERRORS 3


static int parse_decode(struct decoding *d, int argc, char **argv);
static bool capture_option(const char *arg);
static void remove_recording(const char *path);
static int parse_channels(const char *list, uint16_t channels, uint8_t *set);
static void print_status(struct capture *cap, FILE *out);
static void on_event(void *ctx, uint16_t code, uint16_t channel, const uint8_t *data,
                     size_t len);
static void on_signal(int sig);
static uint64_t now_ms(void);
static void usage(FILE *out);


static volatile sig_atomic_t quit;


int capture_main(int argc, char **argv)
{
    struct dummy_options opt = {.realtime = true};
    struct capture_record_options rec = {.binding = BOARD_BINDING, .host = "bus-replay"};
    struct capture_handler handler = {.event = on_event};
    struct capture_config cfg = {0};
    struct capture *cap = NULL;
    const struct capture_info *info = NULL;
    const struct hello *h = NULL;
    struct capture_status st;
    struct sigaction sa = {.sa_handler = on_signal};
    struct decoding dec;
    bool decoding = false;
    const char *out = NULL;
    const char *channels = NULL;
    uint8_t *set = NULL;
    double duration = 0;
    bool dummy = false;
    uint64_t start = 0;
    uint64_t next_status = 0;
    uint64_t tail = 0;
    uint8_t reason = THC_REASON_DURATION;
    char error[256];
    int decode_err = 0;
    int err = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-h", "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(argv[i], "--dummy")) {
            dummy = true;
        } else if (arg_is(argv[i], "-o", "--out") && i + 1 < argc) {
            out = argv[++i];
        } else if (arg_is(argv[i], "-r", "--rate") && i + 1 < argc) {
            cfg.rate = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (arg_is(argv[i], "-C", "--c") && i + 1 < argc) {
            cfg.c = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (arg_is(argv[i], "-c", "--channels") && i + 1 < argc) {
            channels = argv[++i];
        } else if (arg_is(argv[i], "-d", "--duration") && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--decode")) {
            err = parse_decode(&dec, argc - i, argv + i);
            if (err != 0) {
                return err == DECODING_HELP ? 0 : 2;
            }
            decoding = true;
            break;
        } else {
            usage(stderr);
            return 2;
        }
    }
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (decoding) {
        // Items go out as they come; a reader that goes away (| head) ends the capture.
        setvbuf(stdout, NULL, _IOLBF, 0);
        signal(SIGPIPE, SIG_IGN);
        decoding_handler(&dec, &handler);
    }

    if (dummy) {
        rec.binding = "dummy";
        err = capture_open_dummy(&cap, &opt, &handler);
    } else {
        err = capture_open_local(&cap, &handler);
    }
    if (err == 0) {
        err = capture_hello(cap, &h);
    }
    if (err < 0) {
        fprintf(stderr, "bus-replay capture: no device: %s\n", strerror(-err));
        capture_close(cap);
        return 1;
    }
    fprintf(stderr, "hello: %s, firmware %s, spec %u.%u, %u channels, %u data channel%s\n",
            h->model, h->firmware, h->version_major, h->version_minor, h->channels,
            h->data_channels, h->data_channels == 1 ? "" : "s");

    if (channels) {
        set = calloc(((size_t)h->channels + 7) / 8 + 1, 1);
        if (!set || parse_channels(channels, h->channels, set) < 0) {
            fprintf(stderr, "bus-replay capture: bad channel list: %s\n", channels);
            free(set);
            capture_close(cap);
            return 2;
        }
        cfg.enabled = set;
    }

    if (out) {
        err = capture_record(cap, out, &rec);
        if (err < 0) {
            fprintf(stderr, "bus-replay capture: can't create %s: %s\n", out, strerror(-err));
            free(set);
            capture_close(cap);
            return 1;
        }
    }

    err = capture_start(cap, &cfg, error, sizeof(error));
    free(set);
    if (err < 0) {
        fprintf(stderr, "bus-replay capture: start rejected: %s\n", error);
        capture_close(cap);
        return 1;
    }
    info = capture_info(cap);
    fprintf(stderr, "capture: %u Hz, C %u, channels", info->rate, info->c);
    for (unsigned int i = 0; i < info->n; i++) {
        fprintf(stderr, " %u (%s)", info->enabled[i], info->names[info->enabled[i]]);
    }
    fprintf(stderr, "%s%s\n", out ? ", recording to " : "", out ? out : "");
    if (decoding) {
        err = decoding_start(&dec, info, "bus-replay capture");
        if (err < 0) {
            // Nothing was decoded: stop, and remove the file if one was started.
            capture_stop(cap, THC_REASON_ERROR);
            capture_close(cap);
            decoding_free(&dec);
            remove_recording(out);
            return err == -ENOMEM ? 1 : 2;
        }
    }

    start = now_ms();
    next_status = start + STATUS_MS;
    while (!quit && (duration <= 0 || now_ms() - start < (uint64_t)(duration * 1000))) {
        err = capture_pull(cap, POLL_MS);
        if (err < 0) {
            fprintf(stderr, "bus-replay capture: reading the device failed (err: %d)\n", err);
            reason = THC_REASON_ERROR;
            break;
        }
        if (decoding) {
            decoding_drain(&dec);
            if (dec.err) {
                reason = THC_REASON_USER;
                break;
            }
            continue;
        }
        if (now_ms() >= next_status) {
            next_status += STATUS_MS;
            print_status(cap, stderr);
        }
    }
    if (quit) {
        reason = quit == SIGINT ? THC_REASON_USER : THC_REASON_SIGNAL;
    }

    start = now_ms();
    while (reason != THC_REASON_ERROR && now_ms() - start < DRAIN_MS) {
        if (capture_pull(cap, POLL_MS) < 0) {
            break;
        }
        if (decoding) {
            decoding_drain(&dec);
        }
    }
    if (decoding) {
        decode_err = decoding_finish(&dec);
        decoding_free(&dec);
        if (decode_err == -EIO && errno == EPIPE) {
            decode_err = 0;
        } else if (decode_err) {
            fprintf(stderr, "bus-replay capture: decoding failed (err: %d)\n", decode_err);
        }
    }

    print_status(cap, stderr);
    err = capture_stop(cap, reason);
    if (err < 0) {
        fprintf(stderr, "bus-replay capture: the recording failed: %s\n", strerror(-err));
    }
    capture_status(cap, &st);
    tail = capture_tail_mismatches(cap);
    if (tail) {
        fprintf(stderr, "%llu mismatch%s after the last good heartbeat, likely a loss open at "
                        "the stop; not counted as errors\n",
                (unsigned long long)tail, tail == 1 ? "" : "es");
    }
    capture_close(cap);

    if (err < 0 || reason == THC_REASON_ERROR || decode_err) {
        return 1;
    }

    return st.total.crc_errors || st.total.mismatches > tail || st.total.gap_errors ?
               EXIT_ERRORS : 0;
}


/*
 * The decode options after --decode (argv[0]). 0, DECODING_HELP with the help given, or
 * -EINVAL with a message given.
 */
static int parse_decode(struct decoding *d, int argc, char **argv)
{
    int err = decoding_parse(d, argc, argv, true, "bus-replay capture");

    if (err == DECODING_HELP) {
        usage(stdout);
    } else if (err < 0 && d->bad && capture_option(d->bad)) {
        fprintf(stderr, "bus-replay capture: %s after --decode: capture options go before "
                "--decode\n", d->bad);
    } else if (err < 0 && d->bad) {
        fprintf(stderr, "bus-replay capture: %s is not a decode option (bus-replay decode "
                "--help lists them)\n", d->bad);
    } else if (err == 0 && !d->count) {
        fprintf(stderr, "bus-replay capture: --decode needs a decoder, for example --decode "
                "--i2c scl=0,sda=1 (bus-replay decode --help lists them)\n");
        err = -EINVAL;
    }

    return err;
}


// Whether arg is one of capture's own options, given where it can't go.
static bool capture_option(const char *arg)
{
    static const char *const options[] = {"-o", "--out", "-d", "--duration", "-r", "--rate",
                                          "-c", "--channels", "-C", "--c", "--dummy"};

    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if (!strcmp(arg, options[i])) {
            return true;
        }
    }

    return false;
}


static void remove_recording(const char *path)
{
    char index[4096];

    if (!path) {
        return;
    }
    snprintf(index, sizeof(index), "%s.index", path);
    unlink(path);
    unlink(index);
}


// "0,1,3" into a channel set; -EINVAL for a channel the device does not have.
static int parse_channels(const char *list, uint16_t channels, uint8_t *set)
{
    const char *p = list;

    while (*p) {
        char *end = NULL;
        unsigned long ch = strtoul(p, &end, 10);

        if (end == p || ch >= channels) {
            return -EINVAL;
        }
        channel_set_put(set, (unsigned int)ch, true);
        p = *end == ',' ? end + 1 : end;
        if (*end && *end != ',') {
            return -EINVAL;
        }
    }

    return 0;
}


static void print_status(struct capture *cap, FILE *out)
{
    const struct capture_info *info = capture_info(cap);
    struct recorder *rec = capture_recorder(cap);
    struct capture_status st;
    struct recorder_stats rs = {0};

    capture_status(cap, &st);
    if (rec) {
        recorder_stats(rec, &rs);
    }
    fprintf(out,
            "%7.1f s  records %llu  heartbeats %llu  bytes %llu  crc %llu  mismatch %llu  "
            "excused %llu  gap %llu  losses %llu",
            info ? (double)st.now / info->rate : 0.0, (unsigned long long)st.total.records,
            (unsigned long long)st.total.heartbeats, (unsigned long long)st.total.bytes,
            (unsigned long long)st.total.crc_errors, (unsigned long long)st.total.mismatches,
            (unsigned long long)st.total.excused, (unsigned long long)st.total.gap_errors,
            (unsigned long long)st.losses);
    if (rec) {
        fprintf(out, "  file %.2f MB  queued %llu  overflow %llu%s",
                (double)rs.written / 1e6, (unsigned long long)rs.queued,
                (unsigned long long)rs.overflow_bytes, recorder_error(rec) ? "  WRITE ERROR" : "");
    }
    fprintf(out, "\n");
}


static void on_event(void *ctx, uint16_t code, uint16_t channel, const uint8_t *data,
                     size_t len)
{
    (void)ctx;

    if (code == THC_EV_DATA_SILENT && len >= 4) {
        fprintf(stderr, "data channel %u: no data for %llu ms\n", channel,
                (unsigned long long)thc_get_le(data, 4));
    } else if (code == THC_EV_OVERFLOW && len >= 8) {
        fprintf(stderr, "data channel %u: %llu bytes dropped, the host fell behind\n", channel,
                (unsigned long long)thc_get_le(data, 8));
    } else if (code == THC_EV_DATA_ERROR && len >= 4) {
        fprintf(stderr, "data channel %u: %.*s\n", channel, (int)(len - 4), (const char *)&data[4]);
    } else if (code == THC_EV_CONTROL_ERROR) {
        fprintf(stderr, "control: %.*s\n", (int)len, (const char *)data);
    }
}


static void on_signal(int sig)
{
    quit = sig;
}


static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}


static void usage(FILE *out)
{
    fprintf(out,
            "usage: bus-replay capture [-o FILE] [-d S] [-r HZ] [-c LIST] [-C TICKS] [--dummy]\n"
            "                          [--decode DECODER... [-f FORMAT] [-l N] [-x] [-p]]\n"
            "\n"
            "Runs a capture on the device until Ctrl-C (or for --duration), printing its status\n"
            "every second. Without --out nothing is kept: the capture is only checked and counted.\n"
            "Options left out keep the device's current setting, shown on the capture: line.\n"
            "\n"
            "  -o, --out FILE       record to FILE, a new .thcapture file (an existing file is\n"
            "                       never overwritten), with its index as FILE.index\n"
            "  -d, --duration S     stop after S seconds (decimals allowed); 0, the default,\n"
            "                       runs until Ctrl-C\n"
            "  -r, --rate HZ        sample rate in Hz, for example 500000; the device takes its\n"
            "                       base clock divided by a whole number in its range, and\n"
            "                       rejects a rate it can't run, saying why\n"
            "  -c, --channels LIST  channels to capture, by number: 0,1,3\n"
            "  -C, --c TICKS        heartbeat interval in samples, 1 to 134217727; shorter\n"
            "                       catches errors and losses sooner but sends more data\n"
            "      --dummy          a stand-in device instead of the real one: I2C on 0 and 1,\n"
            "                       2 held high, UART 115200 on 3, a clock on 4, a PWM on 5,\n"
            "                       and a loss every 2.5 s\n"
            "      --decode ...     decode while capturing: everything after it is decode's\n"
            "                       options (decoders, -f, -l, -x, -p; bus-replay decode\n"
            "                       --help lists them), so capture's own go before it. The\n"
            "                       items go to stdout as they come, about 1 s behind the\n"
            "                       bus, and the status only at the end. With --out the\n"
            "                       capture is recorded too; without it nothing is kept:\n"
            "                         bus-replay capture -c 0,1 --decode --i2c scl=0,sda=1\n"
            "  -h, --help           this help\n"
            "\n"
            "exit: 0 clean; 1 the device or the file failed (no device, start rejected, the\n"
            "file could not be created); 2 bad arguments; 3 the capture had errors (CRC, gap\n"
            "or unexplained heartbeat mismatches)\n");
}
