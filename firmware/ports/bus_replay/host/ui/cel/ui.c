/*
 * bus-replay ui: the terminal UI, a CEL program (ui/cel/bus_replay.cel) built into the
 * binary. ui/cel/ is the only part of bus-replay that needs CEL; without vendor/CEL-b1 the
 * binary is built without it (Makefile).
 *
 * The UI reaches the device through a link (docs/link-protocol.md). It uses the one on the
 * socket it is given, or the one on the default socket if one runs there; otherwise it
 * starts its own, as a child process it ends on exit. --dummy always starts its own, with
 * the dummy device, on a socket of its own.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "capture/remote.h"
#include "modes/modes.h"
#include "ui/cel/bus_replay_cel.h"
#include "ui/cel/cel.h"
#include "ui/cel/lua_session.h"
#include "ui/session/session.h"


#define LINK_START_MS 5000
#define LINK_POLL_MS 50


static bool link_listening(const char *path);
static pid_t link_spawn(const char *path, bool dummy, char *log, size_t size);
static int script_write(char *path, size_t size);
static void usage(FILE *out);


int ui_main(int argc, char **argv)
{
    char path[64] = "";
    char sock[REMOTE_PATH_MAX] = "";
    char log[PATH_MAX] = "";
    const char *script = NULL;
    char *app_argv[4];
    struct session *s = NULL;
    bool dummy = false;
    pid_t link = -1;
    int err = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-h", "--help")) {
            usage(stdout);
            return 0;
        } else if (arg_is(argv[i], NULL, "--script") && i + 1 < argc) {
            script = argv[++i];
        } else if (arg_is(argv[i], "-s", "--socket") && i + 1 < argc) {
            snprintf(sock, sizeof(sock), "%s", argv[++i]);
        } else if (arg_is(argv[i], NULL, "--dummy")) {
            dummy = true;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (dummy && sock[0]) {
        fprintf(stderr, "bus-replay ui: --dummy starts a link of its own; not with --socket\n");
        return 2;
    }

    // The link: given, already running on the default socket, or a child of ours.
    if (!sock[0]) {
        err = remote_default_path(sock, sizeof(sock));
        if (err < 0) {
            fprintf(stderr, "bus-replay ui: no socket path for the link (err: %d)\n", err);
            return 1;
        }
        if (dummy) {
            char *slash = strrchr(sock, '/');
            size_t dir = slash ? (size_t)(slash - sock) : 0;

            snprintf(sock + dir, sizeof(sock) - dir, "%sbus-replay-dummy-%d.sock",
                     dir ? "/" : "", (int)getpid());
        }
        if (dummy || !link_listening(sock)) {
            link = link_spawn(sock, dummy, log, sizeof(log));
            if (link < 0) {
                fprintf(stderr, "bus-replay ui: couldn't start the link (err: %d), see %s\n",
                        (int)link, log);
                return 1;
            }
        }
    }

    if (!script) {
        err = script_write(path, sizeof(path));
        if (err < 0) {
            fprintf(stderr, "bus-replay ui: couldn't write the UI script (err: %d)\n", err);
        }
        script = path;
    }

    // The session the script's bus_replay module drives, for the whole run.
    if (err == 0) {
        err = session_new(&s, NULL);
        if (err < 0) {
            fprintf(stderr, "bus-replay ui: can't start the session (err: %d)\n", err);
        }
    }
    if (err == 0) {
        app_argv[0] = "--socket";
        app_argv[1] = sock;
        app_argv[2] = "--dummy";
        app_argv[3] = dummy ? "yes" : "no";
        lua_session_set(s);
        err = cel_run(script, 4, app_argv);
        lua_session_set(NULL);
        // Ends the capture, through the link, before the link goes.
        session_free(s);
    }

    if (path[0]) {
        unlink(path);
    }
    if (link > 0) {
        int status = 0;

        kill(link, SIGTERM);
        waitpid(link, &status, 0);
        // The log is for a link that failed; one that ended cleanly leaves none.
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            unlink(log);
        }
    }

    return err < 0 ? 1 : 0;
}


// Whether a link listens on path. The probe's connection ends at once, which the link
// takes as a client leaving.
static bool link_listening(const char *path)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    bool ok = false;
    int fd = 0;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        return false;
    }
    strcpy(addr.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);

    return ok;
}


/*
 * Starts "bus-replay link" on path, its output to a log beside the socket (log gets its
 * path), and waits until it listens. The link must not write over the UI's screen.
 */
static pid_t link_spawn(const char *path, bool dummy, char *log, size_t size)
{
    struct timespec pause = {.tv_nsec = LINK_POLL_MS * 1000000L};
    pid_t pid = 0;
    int status = 0;

    snprintf(log, size, "%s.log", path);
    pid = fork();
    if (pid < 0) {
        return -errno;
    }
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDONLY);
        int log_fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

        dup2(null_fd, STDIN_FILENO);
        dup2(log_fd >= 0 ? log_fd : null_fd, STDOUT_FILENO);
        dup2(log_fd >= 0 ? log_fd : null_fd, STDERR_FILENO);
        execl("/proc/self/exe", "bus-replay", "link", "--socket", path,
              dummy ? "--dummy" : (char *)NULL, (char *)NULL);
        _exit(127);
    }

    for (int waited = 0; waited < LINK_START_MS; waited += LINK_POLL_MS) {
        if (link_listening(path)) {
            return pid;
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return -EIO;
        }
        nanosleep(&pause, NULL);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    return -ETIMEDOUT;
}


// The built-in UI program, to a file the CEL runtime can load.
static int script_write(char *path, size_t size)
{
    size_t done = 0;
    int fd = 0;

    snprintf(path, size, "/tmp/bus-replay-ui-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) {
        path[0] = '\0';
        return -errno;
    }
    while (done < bus_replay_cel_len) {
        ssize_t n = write(fd, bus_replay_cel + done, bus_replay_cel_len - done);

        if (n <= 0) {
            close(fd);
            unlink(path);
            path[0] = '\0';
            return -EIO;
        }
        done += (size_t)n;
    }
    close(fd);

    return 0;
}


static void usage(FILE *out)
{
    fprintf(out,
            "usage: bus-replay ui [--dummy | -s PATH] [--script FILE]\n"
            "\n"
            "The device through a link: the one on PATH, the one on the default socket if one\n"
            "runs there, or else a link of its own, ended on exit.\n"
            "\n"
            "  -s, --socket PATH   the link's socket\n"
            "      --dummy         a link of its own with the dummy device\n"
            "      --script FILE   run the UI from FILE instead of the built-in copy\n");
}
