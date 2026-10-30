/*
 * bus-replay: every mode in one binary (usage()).
 */

#include <stdio.h>
#include <string.h>

#include "modes/modes.h"


struct mode {
    const char *name;
    int (*run)(int argc, char **argv);
};


static void usage(FILE *out);


static const struct mode modes[] = {
    {"ui", ui_main},
    {"link", link_main},
    {"capture", capture_main},
    {"info", info_main},
    {"decode", decode_main},
    {"export", export_main},
};


int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(stdout);
        return 0;
    }
    if (argc < 2) {
        static char *ui_argv[] = {"bus-replay", "ui", NULL};

        argc = 2;
        argv = ui_argv;
    }

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (!strcmp(argv[1], modes[i].name)) {
            if (!modes[i].run) {
                fprintf(stderr,
                        "bus-replay: %s: not in this build (it needs ui/cel/ and vendor/CEL-b1)\n",
                        argv[1]);
                return 1;
            }
            return modes[i].run(argc - 1, argv + 1);
        }
    }

    usage(stderr);
    return 2;
}


static void usage(FILE *out)
{
    fprintf(out,
            "usage: bus-replay [MODE] [OPTIONS]\n"
            "\n"
            "modes:\n"
            "  ui        the terminal UI (the default; needs a build with CEL)\n"
            "  link      the device, served to the UI over a Unix socket\n"
            "  capture   capture from the device, optionally recording to a .thcapture file\n"
            "  info      what a .thcapture file holds: device, settings, per-channel counts\n"
            "  decode    decode a .thcapture file (I2C, UART, ...) to text or JSON\n"
            "  export    a .thcapture file's raw records as CSV or VCD\n"
            "\n"
            "bus-replay MODE -h shows a mode's options. For example:\n"
            "  bus-replay capture -o cap.thcapture -c 0,1,2,3\n"
            "  bus-replay decode cap.thcapture --i2c scl=0,sda=1 --uart tx=3,baud=115200\n");
}
