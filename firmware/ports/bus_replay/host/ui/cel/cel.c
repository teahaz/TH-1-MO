/*
 * The CEL runtime (vendor/CEL-b1, unchanged), built into this binary. CEL builds as one
 * translation unit from runtime.c; this file is that unit, with an entry point in place of
 * CEL's own main.c. It is compiled without warnings, as CEL's code is not ours to change.
 */

#define _XOPEN_SOURCE 700
#define _GNU_SOURCE

#include "runtime.c"

#include "ui/cel/cel.h"


/*
 * Runs the CEL program at path with the program arguments argv. With stdin not a terminal,
 * it runs the scripted inputs from stdin instead and prints the last frame, as cel does.
 */
int cel_run(const char *path, int argc, char **argv)
{
    Runtime rtm = {
        .http = false,
        .location = (char *)path,
        .initial_path = "/",
        .argc = argc,
        .argv = argv,
    };
    bool ok = false;

    if (!rtm_init(&rtm)) {
        return -EINVAL;
    }

    if (!isatty(STDIN_FILENO)) {
        char inputs[4096] = {0};
        size_t length = 0;
        ssize_t n = 0;

        while ((n = read(STDIN_FILENO, inputs + length, sizeof(inputs) - length - 1)) > 0) {
            length += (size_t)n;
            if (length >= sizeof(inputs) - 1) {
                break;
            }
        }
        while (length > 0 && (inputs[length - 1] == '\n' || inputs[length - 1] == '\r')) {
            inputs[--length] = '\0';
        }
        printf("<output>\n");
        ok = rtm_run_inputs(&rtm, inputs);
        printf("</output>\n");
    } else {
        ok = rtm_run(&rtm);
    }
    rtm_destroy(&rtm);

    return ok ? 0 : -EIO;
}
