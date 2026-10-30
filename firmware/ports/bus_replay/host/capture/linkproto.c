/*
 * The link protocol's messages (capture/linkproto.h).
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "capture/linkproto.h"
#include "capture/thcfile.h"


// Appends a whole message, header and payload, to b.
int link_put(struct thc_buf *b, const struct link_msg *m)
{
    int err = 0;

    if (m->len > LINK_PAYLOAD_MAX) {
        return -EMSGSIZE;
    }
    err = thc_buf_le(b, m->type, 1);
    err = err ? err : thc_buf_le(b, m->flags, 1);
    err = err ? err : thc_buf_le(b, m->channel, 2);
    err = err ? err : thc_buf_le(b, m->len, 4);
    err = err ? err : thc_buf_le(b, m->time, 8);
    if (err == 0 && m->len) {
        err = thc_buf_put(b, m->data, m->len);
    }

    return err;
}


/*
 * The message at the start of buf: its size in bytes, 0 if it is not all there yet, or
 * -EPROTO for a length past LINK_PAYLOAD_MAX.
 */
ssize_t link_parse(const uint8_t *buf, size_t len, struct link_msg *m)
{
    uint64_t n = 0;

    if (len < LINK_HEAD) {
        return 0;
    }
    n = thc_get_le(&buf[4], 4);
    if (n > LINK_PAYLOAD_MAX) {
        return -EPROTO;
    }
    if (len < LINK_HEAD + n) {
        return 0;
    }
    m->type = buf[0];
    m->flags = buf[1];
    m->channel = (uint16_t)thc_get_le(&buf[2], 2);
    m->time = thc_get_le(&buf[8], 8);
    m->data = &buf[LINK_HEAD];
    m->len = (size_t)n;

    return (ssize_t)(LINK_HEAD + n);
}


/*
 * The socket's path: given if it is set, else $BUS_REPLAY_LINK, else
 * $XDG_RUNTIME_DIR/bus-replay.sock, else /tmp/bus-replay-UID/link.sock (the directory made
 * with mode 0700 if it is not there).
 */
int link_path(char *buf, size_t size, const char *given)
{
    const char *env = getenv("BUS_REPLAY_LINK");
    const char *run = getenv("XDG_RUNTIME_DIR");
    char dir[64];
    int n = 0;

    if (given && *given) {
        n = snprintf(buf, size, "%s", given);
    } else if (env && *env) {
        n = snprintf(buf, size, "%s", env);
    } else if (run && *run) {
        n = snprintf(buf, size, "%s/bus-replay.sock", run);
    } else {
        snprintf(dir, sizeof(dir), "/tmp/bus-replay-%u", (unsigned int)getuid());
        if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
            return -errno;
        }
        n = snprintf(buf, size, "%s/link.sock", dir);
    }

    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}


// Sends all of data on fd, waiting up to timeout_ms for room each time (-ETIMEDOUT).
int link_send(int fd, const uint8_t *data, size_t len, int timeout_ms)
{
    size_t done = 0;

    while (done < len) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        ssize_t n = 0;

        if (poll(&p, 1, timeout_ms) == 0) {
            return -ETIMEDOUT;
        }
        n = send(fd, data + done, len - done, MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return -errno;
        }
        if (n > 0) {
            done += (size_t)n;
        }
    }

    return 0;
}
