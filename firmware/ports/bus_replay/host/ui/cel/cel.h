/*
 * The CEL runtime (vendor/CEL-b1, unchanged), built into this binary as one translation
 * unit (ui/cel/cel.c).
 */

#ifndef UI_CEL_CEL_H
#define UI_CEL_CEL_H


int cel_run(const char *path, int argc, char **argv);

#endif
