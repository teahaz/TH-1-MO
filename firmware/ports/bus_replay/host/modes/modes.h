/*
 * The command line modes of bus-replay (main.c). Each takes its own arguments, argv[0]
 * being the mode's name, and returns the exit code.
 */

#ifndef MODES_MODES_H
#define MODES_MODES_H

#include <stdbool.h>
#include <string.h>


// Whether arg is the option given by its short form (or NULL for none) or its long form.
static inline bool arg_is(const char *arg, const char *short_name, const char *long_name)
{
    return (short_name && !strcmp(arg, short_name)) || !strcmp(arg, long_name);
}

int capture_main(int argc, char **argv);
int info_main(int argc, char **argv);
int decode_main(int argc, char **argv);
int export_main(int argc, char **argv);

#endif
