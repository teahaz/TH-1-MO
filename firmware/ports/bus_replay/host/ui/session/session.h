/*
 * The session: one background thread that owns a capture for the UI. It is the only code
 * that calls into libcapture for that capture. It pulls from the backend, gathers each
 * pull into a store batch and writes it (ui/store/store.h), runs the decoders over the
 * store, and keeps their items (ui/session/items.h). The UI thread never waits on any of
 * it: it sends commands, reads the status, and queries the store and the items.
 *
 * Commands are queued and run in order in the session thread. Each call returns the
 * command's id at once; the status says which command finished last and how it went.
 * Opening a backend closes the one before. The backends are the device on this Pi
 * (local), the dummy, a capture file, and a link (remote, capture/remote.h); with a link
 * that already runs a capture, open joins it.
 *
 * What the UI reads is a view: the hello, the capture info, the store and the items of the
 * session's current capture. A new hello or a new capture makes a new view; the old one
 * stays valid for whoever acquired it until they release it, so the UI never uses a freed
 * store. A view's gen tells captures apart.
 *
 * Decoders come in groups: a protocol decoder (by name, as the decode mode takes them,
 * codec/codec.h) and the decoders stacked on it. Each group has a codec session of its own
 * and reads the store from where it has got to, so a group added later decodes the history
 * the store still holds, and adding or removing one leaves the others as they are. The
 * store gives what heartbeats would: every grid point's levels (codec_line_level()) in
 * step with the records, and codec_advance(). While the capture runs, a group decodes up
 * to the watermark less its hold, so the losses up to there are known, and each is given
 * just before the first record after its start: a group decodes the same items live and
 * when it catches up, and its items reach the UI its hold after the capture.
 */

#ifndef UI_SESSION_SESSION_H
#define UI_SESSION_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture/control.h"
#include "common/capture_info.h"
#include "common/status.h"
#include "ui/session/items.h"
#include "ui/store/store.h"


#define SESSION_TEXT_MAX 160
#define SESSION_LABEL_MAX 32
#define SESSION_ARGS_MAX 256
#define SESSION_DECODERS_MAX 16
// The most records a group decodes in one go, so catching up never holds up the capture.
#define SESSION_REPLAY_STEP 65536

enum session_backend {
    SESSION_NONE,
    SESSION_LOCAL,
    SESSION_DUMMY,
    SESSION_FILE,
    SESSION_REMOTE,
};

/*
 * CLOSED: no backend. READY: a backend, and its hello if it has one. RUNNING: a capture
 * runs, or a file is being read. ENDED: the capture has ended (stopped, a file read to its
 * end, a backend error); its view stays until the next start or open.
 */
enum session_state {
    SESSION_CLOSED,
    SESSION_READY,
    SESSION_RUNNING,
    SESSION_ENDED,
};

// store_limit: for each capture's store (0: STORE_LIMIT_DEFAULT).
struct session_options {
    size_t store_limit;
};

// path: the capture file, or the link's socket (NULL: the default). realtime: the dummy's.
struct session_open {
    enum session_backend backend;
    const char *path;
    bool realtime;
};

// As struct capture_config, copied when queued. enabled is a channel set, or NULL.
struct session_config {
    uint32_t rate;
    uint32_t c;
    const uint8_t *enabled;
    size_t set_size;
};

/*
 * The session as a whole, copied out. busy: a command is running or queued. done: the id
 * of the last command finished, result how it went (0 or a negative errno), text the
 * reason when it failed (a device's rejection, a backend's error). gen: the current view's.
 * events: the events so far (session_events()). decoded: the tick every group has decoded
 * up to, and catching_up whether a group is still decoding history.
 */
struct session_status {
    enum session_state state;
    enum session_backend backend;
    bool busy;
    uint64_t done;
    int result;
    char text[SESSION_TEXT_MAX];
    uint64_t gen;
    struct capture_status capture;
    uint64_t overflow;
    uint64_t events;
    uint64_t decoded;
    bool catching_up;
};

/*
 * An event of the capture (capture/thcfile.h, THC_EV_*) or of the session (code 0), with
 * the time it happened (ns from the capture's start) and a line of text for it.
 */
struct session_event {
    uint64_t seq;
    uint64_t time;
    uint16_t code;
    uint16_t channel;
    char text[SESSION_TEXT_MAX];
};

/*
 * A decoder group: its label (the protocol decoder's name= or its own name), the protocol
 * decoder as given, how many are stacked on it, its hold, how far it has decoded, and the
 * error that stopped it (0 if none). group is its id in the view's items.
 */
struct session_decoder {
    uint32_t group;
    char label[SESSION_LABEL_MAX];
    char name[SESSION_LABEL_MAX];
    char args[SESSION_ARGS_MAX];
    size_t stacked;
    double hold;
    uint64_t decoded;
    int error;
    char text[SESSION_TEXT_MAX];
};

/*
 * What the UI reads, valid from session_acquire() to session_release(). hello is NULL
 * before the backend has one; info, store and items are NULL before a capture.
 */
struct session_view {
    uint64_t gen;
    const struct hello *hello;
    const struct capture_info *info;
    struct store *store;
    struct items *items;
};

struct session;


int session_new(struct session **out, const struct session_options *opt);
void session_free(struct session *s);

/*
 * Commands. Each returns its id (from 1), or a negative errno if it could not be queued.
 *  - open: the backend, closing the one before (a running capture is stopped).
 *  - hello: asks the backend for its hello again (resets the device).
 *  - start: a capture with cfg (NULL: the device's values). A file starts being read.
 *  - stop: ends the capture; its view stays.
 *  - record: the file the next capture is recorded to (NULL: none); a link records it.
 *    Until it is given, a link records as it was told to on its command line.
 *  - close: the backend goes.
 *  - decoder_add: a protocol decoder, or with on a group's label, one stacked on it. hold
 *    (a new group's) is how long, in seconds of capture, an item is held back for a late
 *    loss report to mark it (0: 1 s); its items reach the UI that much after their end.
 *  - decoder_remove: a group by its label, with what is stacked on it.
 */
int64_t session_open(struct session *s, const struct session_open *o);
int64_t session_hello(struct session *s);
int64_t session_start(struct session *s, const struct session_config *cfg);
int64_t session_stop(struct session *s);
int64_t session_record(struct session *s, const char *path);
int64_t session_close(struct session *s);
int64_t session_decoder_add(struct session *s, const char *name, const char *args,
                            const char *on, double hold);
int64_t session_decoder_remove(struct session *s, const char *label);

void session_status(struct session *s, struct session_status *st);
size_t session_events(struct session *s, uint64_t after, struct session_event *out, size_t max);
size_t session_decoders(struct session *s, struct session_decoder *out, size_t max);

// The current view, or NULL before any; every acquire is released.
struct session_view *session_acquire(struct session *s);
void session_release(struct session *s, struct session_view *v);

#endif
