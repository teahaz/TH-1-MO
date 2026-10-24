/*
 * capture --decode on the dummy: the items decoded live are the items decode gives from the
 * file recorded alongside; without --out nothing is written; and the arguments that don't
 * belong after --decode are refused.
 *
 * Runs the binary (test_bin()) through the shell; files go to a new temporary directory.
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test/test.h"


#define DECODERS "--i2c scl=0,sda=1 --chip sht3x@0x44 --uart tx=3,baud=115200 -f jsonl"


static int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *slurp(const char *path, size_t *len);
static bool dir_empty(const char *path);


static char bin[1024];


int main(void)
{
    const char *dir = test_dir();
    char empty[600];
    char *live = NULL;
    char *file = NULL;
    size_t live_len = 0;
    size_t file_len = 0;

    if (!realpath(test_bin(), bin)) {
        fprintf(stderr, "%s is missing\n", test_bin());
        return 1;
    }

    // Live and from the file: the same items, byte for byte.
    CHECK(run("%s capture --dummy -c 0,1,2,3 -d 3 -o %s/both.thcapture --decode " DECODERS
              " > %s/live.jsonl 2>/dev/null", bin, dir, dir) == 0);
    CHECK(run("%s decode %s/both.thcapture " DECODERS " > %s/file.jsonl", bin, dir, dir) == 0);
    snprintf(empty, sizeof(empty), "%s/live.jsonl", dir);
    live = slurp(empty, &live_len);
    snprintf(empty, sizeof(empty), "%s/file.jsonl", dir);
    file = slurp(empty, &file_len);
    CHECK(live && file && live_len > 1000);
    CHECK(live && file && live_len == file_len && !memcmp(live, file, live_len));
    CHECK(live && strstr(live, "\"decoder\":\"uart\"") && strstr(live, "\"SHT3x\""));
    free(live);
    free(file);

    // Without --out, run from an empty directory: it stays empty.
    snprintf(empty, sizeof(empty), "%s/empty", dir);
    mkdir(empty, 0755);
    CHECK(dir_empty(empty));
    CHECK(run("cd %s && %s capture --dummy -d 1 --decode --i2c scl=0,sda=1 > %s/none.txt "
              "2>/dev/null", empty, bin, dir) == 0);
    CHECK(dir_empty(empty));

    // What --decode refuses, before anything starts.
    CHECK(run("%s capture --dummy --decode --i2c scl=0,sda=1 -o x 2>/dev/null", bin) == 2);
    CHECK(run("%s capture --dummy --decode --i2c scl=0,sda=1 --from 1 2>/dev/null", bin) == 2);
    CHECK(run("%s capture --dummy --decode --i2c scl=0,sda=1 -F 2>/dev/null", bin) == 2);
    CHECK(run("%s capture --dummy --decode -f jsonl 2>/dev/null", bin) == 2);
    CHECK(run("%s capture --dummy --decode --nonsense 2>/dev/null", bin) == 2);
    // A decoder the capture can't have: refused once the capture is known, nothing kept.
    CHECK(run("%s capture --dummy -d 1 -o %s/refused.thcapture --decode --i2c scl=9,sda=1 "
              "2>/dev/null", bin, dir) == 2);
    snprintf(empty, sizeof(empty), "%s/refused.thcapture", dir);
    CHECK(access(empty, F_OK) != 0);

    return test_result(__FILE__);
}


// Its exit code, or -1.
static int run(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    int status = 0;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    status = system(cmd);

    return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long size = 0;

    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)size + 1);
    if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
        buf[size] = '\0';
        *len = (size_t)size;
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);

    return buf;
}


static bool dir_empty(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e = NULL;
    bool empty = true;

    if (!d) {
        return false;
    }
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            empty = false;
        }
    }
    closedir(d);

    return empty;
}
