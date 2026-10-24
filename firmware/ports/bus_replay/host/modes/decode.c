/*
 * bus-replay decode: runs decoders over a capture file and writes their items, as text,
 * JSON lines or a decoder's own export (docs/decode-output.md; the options and the session
 * are in decoding.c, shared with capture --decode). With --follow it keeps reading a file a
 * capture is still writing, until the capture ends or Ctrl-C.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "capture/capture.h"
#include "capture/thcfile.h"
#include "codec/codec.h"
#include "common/capture_info.h"
#include "modes/decoding.h"
#include "modes/modes.h"


#define FOLLOW_MS 200


static void on_signal(int sig);
static void sleep_ms(unsigned int ms);
static void usage(FILE *out);


static volatile sig_atomic_t quit;


int decode_main(int argc, char **argv)
{
    struct decoding d;
    struct capture_handler handler = {0};
    const struct capture_info *info = NULL;
    struct capture *cap = NULL;
    struct sigaction sa = {.sa_handler = on_signal};
    int err = 0;

    err = decoding_parse(&d, argc, argv, false, "bus-replay decode");
    if (err == DECODING_HELP) {
        usage(stdout);
        return 0;
    }
    if (err < 0 || !d.path || !d.count) {
        if (err == 0 || d.bad) {
            usage(stderr);
        }
        return 2;
    }

    decoding_handler(&d, &handler);
    err = capture_open_file(&cap, d.path, &handler);
    if (err < 0) {
        fprintf(stderr, "bus-replay decode: can't read %s: %s\n", d.path,
                err == -EPROTO ? "not a capture file, or its header is damaged" :
                                 strerror(-err));
        return 1;
    }
    info = capture_info(cap);
    err = decoding_start(&d, info, "bus-replay decode");
    if (err < 0) {
        decoding_free(&d);
        capture_close(cap);
        return err == -ENOMEM ? 1 : 2;
    }

    if (d.from > 0) {
        capture_seek(cap, d.from);
    }
    if (d.follow) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    while (!quit && d.err == 0) {
        struct thc_reader *r = capture_reader(cap);

        err = capture_pull(cap, 0);
        decoding_drain(&d);
        if (err > 0) {
            // Past --to by more than a late loss report needs, nothing more is wanted.
            if (d.to != UINT64_MAX && d.tick > d.to + info->rate) {
                break;
            }
            continue;
        }
        if (err != -ENODATA) {
            fprintf(stderr, "bus-replay decode: reading %s failed (err: %d)\n", d.path, err);
            break;
        }
        err = 0;
        if (!d.follow || thc_reader_state(r) == THC_ENDED || thc_reader_state(r) == THC_BAD_CRC) {
            break;
        }
        sleep_ms(FOLLOW_MS);
        thc_reader_resume(r);
    }

    if (err > 0) {
        err = 0;
    }
    if (!err) {
        err = decoding_finish(&d);
        if (err) {
            fprintf(stderr, "bus-replay decode: failed (err: %d)\n", err);
        }
    } else {
        decoding_finish(&d);
    }
    decoding_free(&d);
    capture_close(cap);

    return err ? 1 : 0;
}


static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}


static void sleep_ms(unsigned int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}


static void usage(FILE *out)
{
    fprintf(out,
            "usage: bus-replay decode FILE DECODER... [-f FORMAT] [-l N] [-x] [-p] [-F]\n"
            "                         [--from S] [--to S]\n"
            "\n"
            "Decodes a .thcapture file and writes the items to stdout, in time order. Give one\n"
            "or more decoders, each with its settings as key=value pairs joined by commas:\n"
            "  bus-replay decode cap.thcapture --i2c scl=0,sda=1 --uart tx=3,baud=115200\n"
            "\n"
            "decoders:\n");
    for (unsigned int i = 0; codec_decoders[i]; i++) {
        const struct codec_decoder *def = codec_decoders[i];

        if (def->input) {
            continue;
        }
        fprintf(out, "  --%-10s %s\n", def->name, def->summary);
        for (unsigned int p = 0; def->params && def->params[p].name; p++) {
            fprintf(out, "      %-8s %s\n", def->params[p].name, def->params[p].help);
        }
        for (unsigned int e = 0; def->exports && def->exports[e].name; e++) {
            fprintf(out, "      -f %s: %s\n", def->exports[e].name, def->exports[e].help);
        }
    }
    fprintf(out,
            "  every decoder also takes name=LABEL, its label in the output (for two of a kind)\n"
            "\n"
            "chip-level decoders, each a reading of the I2C traffic to one address, shown under\n"
            "the transaction it reads; give --chip NAME@ADDR, then any settings after a comma:\n"
            "  bus-replay decode cap.thcapture --i2c scl=0,sda=1 --chip sht3x@0x44 \\\n"
            "                                  --chip ina219@0x43,shunt=0.01,cal=4096\n");
    for (unsigned int i = 0; codec_decoders[i]; i++) {
        const struct codec_decoder *def = codec_decoders[i];

        if (!def->input) {
            continue;
        }
        fprintf(out, "  %-12s %s\n", def->name, def->summary);
        for (unsigned int p = 0; def->params && def->params[p].name; p++) {
            if (strcmp(def->params[p].name, "addr") && strcmp(def->params[p].name, "on")) {
                fprintf(out, "      %-8s %s\n", def->params[p].name, def->params[p].help);
            }
        }
    }
    fprintf(out,
            "  every chip also takes on=LABEL, the i2c decoder it reads (default the last one)\n"
            "\n"
            "options:\n"
            "  -f, --format F  text (the default, for reading), jsonl (one JSON object per\n"
            "                  top-level item with everything under it, for scripts), or an\n"
            "                  export a decoder lists above\n"
            "  -l, --level N   how deep the text goes: 0 top-level items only (the default),\n"
            "                  1 and up add their parts, such as I2C segments; jsonl has all\n"
            "  -x, --hex       show data as hex, also where it reads as text (UART)\n"
            "  -p, --pretty    text with one line per reading: a chip's reading in place of\n"
            "                  the transaction it reads, an I2C transaction no chip reads as\n"
            "                  its header alone, a UART burst as its text alone; no --level\n"
            "      --from S    start S seconds into the capture (decimals allowed)\n"
            "      --to S      stop S seconds into the capture\n"
            "  -F, --follow    keep reading a file a capture is still writing, until that\n"
            "                  capture ends or Ctrl-C; output trails the capture by about 1 s\n"
            "  -h, --help      this help\n");
}
