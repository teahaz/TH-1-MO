/*
 * The session (ui/session/session.h). The UI thread and the session thread share the
 * command queue, the status, the events, the decoder list and the current view, under
 * lock. Everything else belongs to the session thread: the capture, the batch, and the
 * decoder groups' codec sessions.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "capture/board.h"
#include "capture/capture.h"
#include "capture/control.h"
#include "capture/dummy.h"
#include "capture/info.h"
#include "capture/remote.h"
#include "capture/thcfile.h"
#include "codec/codec.h"
#include "codec/item.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "common/status.h"
#include "ui/session/items.h"
#include "ui/session/session.h"
#include "ui/store/store.h"


#define EVENTS_MAX 256
#define STACKED_MAX 8
#define FED_MAX 64
#define READ_MAX 4096
#define IDLE_MS 50
#define PULL_MS 10
#define LATE_S 2
#define KNOWN_LINES 64

enum cmd_type {
    CMD_OPEN,
    CMD_HELLO,
    CMD_START,
    CMD_STOP,
    CMD_RECORD,
    CMD_CLOSE,
    CMD_DECODER_ADD,
    CMD_DECODER_REMOVE,
};

// A command, with copies of everything it was given.
struct cmd {
    uint64_t id;
    enum cmd_type type;
    enum session_backend backend;
    bool realtime;
    bool have_path;
    char path[512];
    struct session_config cfg;
    uint8_t set[64];
    bool have_set;
    char name[SESSION_LABEL_MAX];
    char args[SESSION_ARGS_MAX];
    char on[SESSION_LABEL_MAX];
    double hold;
    struct cmd *next;
};

// A view and what it owns. refs counts the session's hold on it and every acquire.
struct view {
    struct session_view v;
    unsigned int refs;
    struct hello hello;
    bool have_hello;
    struct capture_info info;
    bool have_info;
};

// A loss the store has and the group has not been given yet, with its counts.
struct known {
    struct store_loss loss;
    bool fed;
    uint32_t lost[KNOWN_LINES];
};

// A decoder as the UI gave it.
struct dec_spec {
    char name[SESSION_LABEL_MAX];
    char args[SESSION_ARGS_MAX];
};

/*
 * A decoder group, in the session thread. specs[0] is its protocol decoder, the rest are
 * stacked on it. With a capture, codec decodes it: pos are its merged read positions, skip
 * the tick its records start after, decoded how far it has gone, fed the losses given.
 */
struct group {
    uint32_t id;
    char label[SESSION_LABEL_MAX];
    struct dec_spec specs[STACKED_MAX];
    size_t spec_count;
    double hold;

    struct codec *codec;
    uint64_t *pos;
    bool started;
    uint64_t skip;
    bool have_decoded;
    uint64_t decoded;
    struct store_loss fed[FED_MAX];
    size_t fed_len;
    struct known known[FED_MAX];
    size_t known_len;
    uint16_t n;
    struct record pend[READ_MAX];
    size_t pend_len;
    uint64_t next_level;
    int error;
    char text[SESSION_TEXT_MAX];
};

struct session {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool quit;
    struct session_options opt;

    // Under lock.
    struct cmd *head;
    struct cmd *tail;
    uint64_t next_id;
    unsigned int running_cmds;
    struct session_status st;
    struct session_event events[EVENTS_MAX];
    uint64_t event_count;
    struct session_decoder decs[SESSION_DECODERS_MAX];
    size_t dec_count;
    struct view *cur;
    uint64_t gen;

    // The session thread's.
    struct capture *cap;
    enum session_backend backend;
    bool realtime;
    char path[512];
    bool have_path;
    bool reading;
    char record[512];
    bool record_set;
    struct store_batch *batch;
    struct store *store;
    struct items *items;
    const struct capture_info *info;
    struct group groups[SESSION_DECODERS_MAX];
    size_t group_count;
    uint32_t next_group;
    bool catching_up;
};


static void *session_main(void *arg);
static int64_t queue(struct session *s, struct cmd *c);
static void run(struct session *s, struct cmd *c);
static void done(struct session *s, const struct cmd *c, int result, const char *text);
static int backend_open(struct session *s, char *text, size_t size);
static void backend_close(struct session *s);
static int capture_begin(struct session *s);
static void capture_ended(struct session *s);
static int view_new(struct session *s, bool with_capture);
static void view_put(struct view *v);
static int group_add(struct session *s, const struct cmd *c, char *text, size_t size);
static int group_remove(struct session *s, const char *label);
static int group_start(struct session *s, struct group *g);
static void group_stop(struct group *g);
static bool group_step(struct session *s, struct group *g);
static void groups_publish(struct session *s);
static void status_publish(struct session *s);
static void event_add(struct session *s, uint64_t time, uint16_t code, uint16_t channel,
                      const char *text);
static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static void on_loss(void *ctx, const struct loss *loss);
static void on_event(void *ctx, uint16_t code, uint16_t channel, const uint8_t *data,
                     size_t len);
static void label_of(const char *name, const char *args, char *out, size_t size);


int session_new(struct session **out, const struct session_options *opt)
{
    struct session *s = calloc(1, sizeof(*s));
    int err = 0;

    if (!s) {
        return -ENOMEM;
    }
    if (opt) {
        s->opt = *opt;
    }
    s->next_id = 1;
    s->next_group = 1;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    err = -pthread_create(&s->thread, NULL, session_main, s);
    if (err < 0) {
        pthread_cond_destroy(&s->wake);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return err;
    }
    *out = s;

    return 0;
}


// Stops the thread, closes the backend (a running capture is stopped) and frees it all.
void session_free(struct session *s)
{
    if (!s) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    s->quit = true;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread, NULL);

    while (s->head) {
        struct cmd *c = s->head;

        s->head = c->next;
        free(c);
    }
    if (s->cur) {
        view_put(s->cur);
    }
    pthread_cond_destroy(&s->wake);
    pthread_mutex_destroy(&s->lock);
    free(s);
}


int64_t session_open(struct session *s, const struct session_open *o)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    c->type = CMD_OPEN;
    c->backend = o->backend;
    c->realtime = o->realtime;
    if (o->path) {
        c->have_path = true;
        snprintf(c->path, sizeof(c->path), "%s", o->path);
    }

    return queue(s, c);
}


int64_t session_hello(struct session *s)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    c->type = CMD_HELLO;

    return queue(s, c);
}


int64_t session_start(struct session *s, const struct session_config *cfg)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    if (cfg && cfg->enabled && cfg->set_size > sizeof(c->set)) {
        free(c);
        return -EINVAL;
    }
    c->type = CMD_START;
    if (cfg) {
        c->cfg = *cfg;
        if (cfg->enabled) {
            memcpy(c->set, cfg->enabled, cfg->set_size);
            c->have_set = true;
        }
    }

    return queue(s, c);
}


int64_t session_stop(struct session *s)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    c->type = CMD_STOP;

    return queue(s, c);
}


int64_t session_record(struct session *s, const char *path)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    c->type = CMD_RECORD;
    snprintf(c->path, sizeof(c->path), "%s", path ? path : "");

    return queue(s, c);
}


int64_t session_close(struct session *s)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    c->type = CMD_CLOSE;

    return queue(s, c);
}


int64_t session_decoder_add(struct session *s, const char *name, const char *args,
                            const char *on, double hold)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    if (!name || strlen(name) >= sizeof(c->name) || (args && strlen(args) >= sizeof(c->args)) ||
        (on && strlen(on) >= sizeof(c->on))) {
        free(c);
        return -EINVAL;
    }
    c->type = CMD_DECODER_ADD;
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->args, sizeof(c->args), "%s", args ? args : "");
    snprintf(c->on, sizeof(c->on), "%s", on ? on : "");
    c->hold = hold;

    return queue(s, c);
}


int64_t session_decoder_remove(struct session *s, const char *label)
{
    struct cmd *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    if (!label || strlen(label) >= sizeof(c->name)) {
        free(c);
        return -EINVAL;
    }
    c->type = CMD_DECODER_REMOVE;
    snprintf(c->name, sizeof(c->name), "%s", label);

    return queue(s, c);
}


void session_status(struct session *s, struct session_status *st)
{
    pthread_mutex_lock(&s->lock);
    *st = s->st;
    st->busy = s->head || s->running_cmds;
    st->gen = s->cur ? s->cur->v.gen : 0;
    st->events = s->event_count;
    pthread_mutex_unlock(&s->lock);
}


size_t session_events(struct session *s, uint64_t after, struct session_event *out, size_t max)
{
    size_t n = 0;

    pthread_mutex_lock(&s->lock);
    for (uint64_t seq = after + 1; seq <= s->event_count && n < max; seq++) {
        if (s->event_count - seq < EVENTS_MAX) {
            out[n++] = s->events[(seq - 1) % EVENTS_MAX];
        }
    }
    pthread_mutex_unlock(&s->lock);

    return n;
}


size_t session_decoders(struct session *s, struct session_decoder *out, size_t max)
{
    size_t n = 0;

    pthread_mutex_lock(&s->lock);
    for (; n < s->dec_count && n < max; n++) {
        out[n] = s->decs[n];
    }
    pthread_mutex_unlock(&s->lock);

    return n;
}


struct session_view *session_acquire(struct session *s)
{
    struct session_view *v = NULL;

    pthread_mutex_lock(&s->lock);
    if (s->cur) {
        s->cur->refs++;
        v = &s->cur->v;
    }
    pthread_mutex_unlock(&s->lock);

    return v;
}


void session_release(struct session *s, struct session_view *v)
{
    if (!v) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    view_put((struct view *)v);
    pthread_mutex_unlock(&s->lock);
}


/*
 * The session thread: runs the commands, then pulls from the backend into the store and
 * lets every group decode a step, and waits only when there is nothing to do.
 */
static void *session_main(void *arg)
{
    struct session *s = arg;

    for (;;) {
        struct cmd *c = NULL;
        bool work = false;

        pthread_mutex_lock(&s->lock);
        if (s->quit) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        c = s->head;
        if (c) {
            s->head = c->next;
            if (!s->head) {
                s->tail = NULL;
            }
            s->running_cmds++;
        }
        pthread_mutex_unlock(&s->lock);
        if (c) {
            run(s, c);
            pthread_mutex_lock(&s->lock);
            s->running_cmds--;
            pthread_mutex_unlock(&s->lock);
            free(c);
            continue;
        }

        if (s->cap && s->reading) {
            int n = capture_pull(s->cap, s->backend == SESSION_FILE ? 0 : PULL_MS);
            int err = store_write(s->store, s->batch);

            store_batch_clear(s->batch);
            if (err < 0) {
                event_add(s, 0, 0, 0, "the store could not take everything");
            }
            if (n == -ENODATA) {
                event_add(s, capture_time(s->cap), 0, 0, "the end of the file");
                capture_ended(s);
            } else if (n < 0) {
                char text[SESSION_TEXT_MAX];

                snprintf(text, sizeof(text), "the backend failed (err: %d)", n);
                event_add(s, capture_time(s->cap), 0, 0, text);
                capture_ended(s);
            }
            work = true;
        }
        s->catching_up = false;
        for (size_t i = 0; i < s->group_count; i++) {
            work = group_step(s, &s->groups[i]) || work;
        }
        if (s->store && s->items) {
            struct store_stats st;

            store_stats(s->store, &st);
            items_trim(s->items, st.oldest);
        }
        groups_publish(s);
        status_publish(s);

        if (!work) {
            struct timespec until;

            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += IDLE_MS * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&s->lock);
            if (!s->head && !s->quit) {
                pthread_cond_timedwait(&s->wake, &s->lock, &until);
            }
            pthread_mutex_unlock(&s->lock);
        }
    }

    backend_close(s);
    for (size_t i = 0; i < s->group_count; i++) {
        group_stop(&s->groups[i]);
    }
    store_batch_free(s->batch);
    s->batch = NULL;

    return NULL;
}


static int64_t queue(struct session *s, struct cmd *c)
{
    int64_t id = 0;

    pthread_mutex_lock(&s->lock);
    id = (int64_t)s->next_id++;
    c->id = (uint64_t)id;
    if (s->tail) {
        s->tail->next = c;
    } else {
        s->head = c;
    }
    s->tail = c;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);

    return id;
}


static void run(struct session *s, struct cmd *c)
{
    char text[SESSION_TEXT_MAX] = "";
    int err = 0;

    switch (c->type) {
    case CMD_OPEN:
        backend_close(s);
        s->backend = c->backend;
        s->realtime = c->realtime;
        s->have_path = c->have_path;
        snprintf(s->path, sizeof(s->path), "%s", c->path);
        err = backend_open(s, text, sizeof(text));
        break;
    case CMD_HELLO:
        if (s->backend == SESSION_NONE) {
            err = -ENODEV;
            break;
        }
        backend_close(s);
        err = backend_open(s, text, sizeof(text));
        break;
    case CMD_START:
        if (!s->cap) {
            err = -ENODEV;
            snprintf(text, sizeof(text), "no backend");
            break;
        }
        // A session runs one capture: the next one is on a session opened again.
        if (s->reading || capture_info(s->cap) || s->backend == SESSION_FILE) {
            backend_close(s);
            err = backend_open(s, text, sizeof(text));
            if (err < 0 || s->reading) {
                break;
            }
        }
        if (s->backend == SESSION_FILE) {
            err = capture_begin(s);
            break;
        }
        if (s->record[0]) {
            if (s->backend == SESSION_REMOTE) {
                err = remote_record(capture_source(s->cap), s->record);
            } else {
                struct capture_record_options ro = {
                    .binding = s->backend == SESSION_DUMMY ? "dummy" : BOARD_BINDING,
                    .host = "bus-replay ui"};

                err = capture_record(s->cap, s->record, &ro);
            }
            if (err < 0) {
                snprintf(text, sizeof(text), "can't record to %.100s (err: %d)", s->record, err);
                break;
            }
        } else if (s->backend == SESSION_REMOTE && s->record_set) {
            // Turned off from the UI: the link's own setting (its --record) goes too.
            remote_record(capture_source(s->cap), NULL);
        }
        {
            struct capture_config cfg = {.rate = c->cfg.rate, .c = c->cfg.c,
                                         .enabled = c->have_set ? c->set : NULL};

            err = capture_start(s->cap, &cfg, text, sizeof(text));
        }
        err = err ? err : capture_begin(s);
        break;
    case CMD_STOP:
        if (s->cap && s->reading) {
            if (s->backend != SESSION_FILE) {
                err = capture_stop(s->cap, THC_REASON_USER);
            }
            capture_ended(s);
        }
        break;
    case CMD_RECORD:
        snprintf(s->record, sizeof(s->record), "%s", c->path);
        s->record_set = true;
        break;
    case CMD_CLOSE:
        backend_close(s);
        s->backend = SESSION_NONE;
        break;
    case CMD_DECODER_ADD:
        err = group_add(s, c, text, sizeof(text));
        break;
    case CMD_DECODER_REMOVE:
        err = group_remove(s, c->name);
        if (err < 0) {
            snprintf(text, sizeof(text), "no decoder %s", c->name);
        }
        break;
    }
    if (err < 0 && !text[0]) {
        snprintf(text, sizeof(text), "%s", strerror(-err));
    }
    groups_publish(s);
    status_publish(s);
    done(s, c, err, text);
}


static void done(struct session *s, const struct cmd *c, int result, const char *text)
{
    pthread_mutex_lock(&s->lock);
    s->st.done = c->id;
    s->st.result = result;
    snprintf(s->st.text, sizeof(s->st.text), "%s", result < 0 ? text : "");
    pthread_mutex_unlock(&s->lock);
}


/*
 * Opens s->backend, with a new view for its hello. A link that runs a capture is joined,
 * so the capture's view comes with it.
 */
static int backend_open(struct session *s, char *text, size_t size)
{
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .loss = on_loss, .event = on_event, .ctx = s};
    int err = 0;

    switch (s->backend) {
    case SESSION_LOCAL:
        err = capture_open_local(&s->cap, &h);
        break;
    case SESSION_DUMMY: {
        struct dummy_options o = {.realtime = s->realtime};

        err = capture_open_dummy(&s->cap, &o, &h);
        break;
    }
    case SESSION_FILE:
        err = s->have_path ? capture_open_file(&s->cap, s->path, &h) : -EINVAL;
        break;
    case SESSION_REMOTE: {
        struct remote_options o = {.path = s->have_path && s->path[0] ? s->path : NULL};

        err = capture_open_remote(&s->cap, &o, &h);
        break;
    }
    default:
        err = -EINVAL;
        break;
    }
    if (err < 0) {
        s->cap = NULL;
        snprintf(text, size, "can't open the backend (err: %d)", err);
        return err;
    }

    if (s->backend == SESSION_REMOTE && remote_running(capture_source(s->cap))) {
        struct capture_attach a;

        err = remote_attach(capture_source(s->cap), &a);
        err = err ? err : capture_attach(s->cap, &a, text, size);
        err = err ? err : capture_begin(s);
        if (err < 0 && !text[0]) {
            snprintf(text, size, "can't join the link's capture (err: %d)", err);
        }
        return err;
    }

    return view_new(s, false);
}


static void backend_close(struct session *s)
{
    if (s->reading) {
        if (s->backend != SESSION_FILE) {
            capture_stop(s->cap, THC_REASON_USER);
        }
        capture_ended(s);
    }
    capture_close(s->cap);
    s->cap = NULL;
}


// The capture runs (or the file is read): a new view, with a store, and the groups on it.
static int capture_begin(struct session *s)
{
    int err = view_new(s, true);

    if (err < 0) {
        return err;
    }
    store_batch_free(s->batch);
    s->batch = NULL;
    err = store_batch_new(&s->batch, s->info);
    if (err < 0) {
        return err;
    }
    for (size_t i = 0; i < s->group_count; i++) {
        group_stop(&s->groups[i]);
        group_start(s, &s->groups[i]);
    }
    s->reading = true;
    event_add(s, 0, 0, 0, s->backend == SESSION_FILE ? "reading the file" : "capture started");

    return 0;
}


// The capture is over; its view stays, and the groups decode what is left in the store.
static void capture_ended(struct session *s)
{
    s->reading = false;
}


/*
 * A new current view: the backend's hello, and with with_capture the capture's info and a
 * new store and item cache. The old one goes once its last reader lets go of it.
 */
static int view_new(struct session *s, bool with_capture)
{
    struct view *v = calloc(1, sizeof(*v));
    struct store_options so = {.limit = s->opt.store_limit};
    const uint8_t *msg = NULL;
    size_t len = 0;
    int err = 0;

    if (!v) {
        return -ENOMEM;
    }
    // The groups' codec sessions read the old view's info: they go first.
    for (size_t i = 0; i < s->group_count; i++) {
        group_stop(&s->groups[i]);
    }
    v->refs = 1;
    if (s->cap && capture_hello_msg(s->cap, &msg, &len) == 0 &&
        hello_parse(msg, len, &v->hello) == 0) {
        v->have_hello = true;
        v->v.hello = &v->hello;
    }
    if (with_capture) {
        err = capture_info_copy(&v->info, capture_info(s->cap));
        v->have_info = err == 0;
        v->v.info = &v->info;
        err = err ? err : store_new(&v->v.store, &v->info, &so);
        err = err ? err : items_new(&v->v.items);
        if (err < 0) {
            pthread_mutex_lock(&s->lock);
            view_put(v);
            pthread_mutex_unlock(&s->lock);
            return err;
        }
    }

    pthread_mutex_lock(&s->lock);
    v->v.gen = ++s->gen;
    if (s->cur) {
        view_put(s->cur);
    }
    s->cur = v;
    pthread_mutex_unlock(&s->lock);
    if (with_capture) {
        s->store = v->v.store;
        s->items = v->v.items;
        s->info = v->v.info;
    } else {
        s->store = NULL;
        s->items = NULL;
        s->info = NULL;
    }

    return 0;
}


// Lets go of one hold on v, under the session's lock; the last one frees it.
static void view_put(struct view *v)
{
    if (--v->refs > 0) {
        return;
    }
    items_free(v->v.items);
    store_free(v->v.store);
    if (v->have_info) {
        capture_info_free(&v->info);
    }
    if (v->have_hello) {
        hello_free(&v->hello);
    }
    free(v);
}


/*
 * A protocol decoder as a new group, or with c->on one stacked on that group, which then
 * decodes again from the start. With a capture, the decoders are made at once, so an error
 * in their arguments shows in the command's result.
 */
static int group_add(struct session *s, const struct cmd *c, char *text, size_t size)
{
    struct group *g = NULL;
    const struct codec_decoder *def = codec_find(c->name);

    if (!def) {
        snprintf(text, size, "no decoder %s", c->name);
        return -ENOENT;
    }
    if (c->on[0]) {
        for (size_t i = 0; i < s->group_count && !g; i++) {
            if (!strcmp(s->groups[i].label, c->on)) {
                g = &s->groups[i];
            }
        }
        if (!g) {
            snprintf(text, size, "no decoder %s", c->on);
            return -ENOENT;
        }
        if (g->spec_count == STACKED_MAX || !def->input) {
            snprintf(text, size, "%s can't be stacked on %s", c->name, c->on);
            return -EINVAL;
        }
    } else {
        char label[SESSION_LABEL_MAX];

        if (def->input) {
            snprintf(text, size, "%s goes on another decoder", c->name);
            return -EINVAL;
        }
        if (s->group_count == SESSION_DECODERS_MAX) {
            snprintf(text, size, "at most %d decoders", SESSION_DECODERS_MAX);
            return -ENOSPC;
        }
        label_of(c->name, c->args, label, sizeof(label));
        for (size_t i = 0; i < s->group_count; i++) {
            if (!strcmp(s->groups[i].label, label)) {
                snprintf(text, size, "a decoder %s already", label);
                return -EEXIST;
            }
        }
        g = &s->groups[s->group_count++];
        memset(g, 0, sizeof(*g));
        g->id = s->next_group++;
        snprintf(g->label, sizeof(g->label), "%s", label);
        g->hold = c->hold > 0 ? c->hold : 1.0;
    }
    snprintf(g->specs[g->spec_count].name, sizeof(g->specs[0].name), "%s", c->name);
    snprintf(g->specs[g->spec_count].args, sizeof(g->specs[0].args), "%s", c->args);
    g->spec_count++;

    group_stop(g);
    if (s->items) {
        items_clear(s->items, g->id);
    }
    if (s->store && group_start(s, g) < 0) {
        int err = g->error;

        snprintf(text, size, "%s", g->text);
        if (!c->on[0]) {
            s->group_count--;
        } else {
            g->spec_count--;
            g->error = 0;
            g->text[0] = '\0';
            group_start(s, g);
        }
        return err;
    }

    return 0;
}


static int group_remove(struct session *s, const char *label)
{
    for (size_t i = 0; i < s->group_count; i++) {
        if (!strcmp(s->groups[i].label, label)) {
            group_stop(&s->groups[i]);
            if (s->items) {
                items_clear(s->items, s->groups[i].id);
            }
            memmove(&s->groups[i], &s->groups[i + 1],
                    (s->group_count - i - 1) * sizeof(s->groups[0]));
            s->group_count--;
            return 0;
        }
    }

    return -ENOENT;
}


// A codec session for the group over the current capture, reading the store from its start.
static int group_start(struct session *s, struct group *g)
{
    struct codec_dec *lower = NULL;
    char error[CODEC_ERROR_LEN] = "";
    // The session holds the group's input back by its hold (group_step()), not codec.
    int err = codec_new(&g->codec, s->info, 0);

    g->error = 0;
    g->text[0] = '\0';
    for (size_t i = 0; i < g->spec_count && err == 0; i++) {
        struct codec_dec *d = NULL;

        err = codec_add(g->codec, codec_find(g->specs[i].name), g->specs[i].args,
                        i ? lower : NULL, &d, error, sizeof(error));
        if (i == 0) {
            lower = d;
        }
    }
    g->pos = err == 0 ? calloc(s->info->n ? s->info->n : 1, sizeof(*g->pos)) : NULL;
    if (err == 0 && !g->pos) {
        err = -ENOMEM;
    }
    if (err < 0) {
        g->error = err;
        snprintf(g->text, sizeof(g->text), "%s", error[0] ? error : strerror(-err));
        group_stop(g);
        return err;
    }
    g->started = false;
    g->have_decoded = false;
    g->fed_len = 0;
    g->known_len = 0;
    g->pend_len = 0;
    g->n = s->info->n;

    return 0;
}


static void group_stop(struct group *g)
{
    codec_free(g->codec);
    g->codec = NULL;
    free(g->pos);
    g->pos = NULL;
}


// Gives the group every known loss starting before tick (all of them for UINT64_MAX).
static void losses_before(struct group *g, uint64_t tick)
{
    for (size_t i = 0; i < g->known_len; i++) {
        struct known *k = &g->known[i];

        if (!k->fed && k->loss.start < tick) {
            struct loss l = {.start = k->loss.start, .end = k->loss.end, .n = g->n,
                             .lost = k->lost};

            codec_loss(g->codec, &l);
            k->fed = true;
            if (g->fed_len == FED_MAX) {
                memmove(g->fed, g->fed + 1, (FED_MAX - 1) * sizeof(g->fed[0]));
                g->fed_len--;
            }
            g->fed[g->fed_len++] = k->loss;
        }
    }
}


// The store's losses from the group's last tick (less the late window) to upto, not given yet.
static void losses_known(struct session *s, struct group *g, uint64_t upto)
{
    struct store_loss losses[FED_MAX];
    uint32_t lost[FED_MAX * KNOWN_LINES];
    uint64_t late = (uint64_t)LATE_S * s->info->rate;
    uint64_t from = g->have_decoded && g->decoded > late ? g->decoded - late : 0;
    size_t count = 0;

    g->known_len = 0;
    if (s->info->n > KNOWN_LINES ||
        store_losses(s->store, from, upto, losses, lost, FED_MAX, &count) < 0) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        bool seen = false;

        for (size_t k = 0; k < g->fed_len && !seen; k++) {
            seen = g->fed[k].start == losses[i].start && g->fed[k].end == losses[i].end;
        }
        if (!seen) {
            struct known *k = &g->known[g->known_len++];

            k->loss = losses[i];
            k->fed = false;
            memcpy(k->lost, &lost[i * s->info->n], s->info->n * sizeof(uint32_t));
        }
    }
}


/*
 * The levels a heartbeat would give at every grid point before tick, as the store has
 * them: in step with the records, as the device's heartbeats would be, so a level that
 * corrects the records reaches the decoders at the same place however the group is fed.
 */
static void levels_before(struct session *s, struct group *g, uint64_t tick)
{
    const struct capture_info *info = s->info;

    while (g->next_level < tick) {
        for (uint16_t i = 0; i < info->n; i++) {
            int level = store_level(s->store, info->enabled[i], g->next_level);

            if (level == STORE_LOW || level == STORE_HIGH) {
                codec_line_level(g->codec, g->next_level, info->enabled[i], (uint8_t)level);
            }
        }
        g->next_level += info->c;
    }
}


// Whether a known loss not given yet starts before tick.
static bool loss_due(const struct group *g, uint64_t tick)
{
    for (size_t i = 0; i < g->known_len; i++) {
        if (!g->known[i].fed && g->known[i].loss.start < tick) {
            return true;
        }
    }

    return false;
}


/*
 * A record, after the grid points' levels and the losses before it. A loss reaches the
 * decoders at once, while records wait in the codec's merge until it advances: so it
 * advances first, and the decoders have taken every record before the loss.
 */
static void feed(struct session *s, struct group *g, const struct record *r)
{
    levels_before(s, g, r->tick);
    if (loss_due(g, r->tick) && r->tick > 0) {
        codec_advance(g->codec, r->tick - 1);
        losses_before(g, r->tick);
    }
    if (g->skip == 0 || r->tick > g->skip) {
        codec_record(g->codec, r);
    }
}


/*
 * One step of a group, up to its target: the watermark less its hold while the capture
 * runs, so that the losses up to there are known as a rule, and decoding gives the same
 * items live and when catching up; the watermark itself once it has ended. The store's
 * records from where the group got to (at most SESSION_REPLAY_STEP; those past the target
 * wait in pend), each loss just before the first record after its start and the levels of
 * every grid point in step with them, and the items that are ready. True if it did any
 * work.
 */
static bool group_step(struct session *s, struct group *g)
{
    const struct capture_info *info = s->info;
    struct store_stats st;
    struct record buf[READ_MAX];
    struct codec_item *it = NULL;
    uint64_t hold = 0;
    uint64_t target = 0;
    uint64_t upto = 0;
    size_t total = 0;
    size_t keep = 0;
    bool all = false;

    if (!g->codec || !s->store) {
        return false;
    }
    store_stats(s->store, &st);
    // Once the capture has ended no loss can come any more: everything can be decoded.
    hold = s->reading ? (uint64_t)(g->hold * info->rate) : 0;
    if (!st.have_watermark || st.watermark < hold) {
        return false;
    }
    target = st.watermark - hold;
    if (g->have_decoded && g->decoded >= target) {
        return false;
    }

    if (!g->started) {
        for (uint16_t i = 0; i < info->n; i++) {
            struct store_channel_stats cs;

            store_channel_stats(s->store, info->enabled[i], &cs);
            g->pos[i] = cs.first;
        }
        g->skip = st.oldest;
        g->pend_len = 0;
        // From the first grid point the store holds the levels of.
        g->next_level = (st.oldest + info->c - 1) / info->c * info->c;
        g->started = true;
    }

    // Losses that came after the group had passed them are given at once.
    losses_known(s, g, target);
    losses_before(g, g->have_decoded ? g->decoded + 1 : 0);

    for (size_t i = 0; i < g->pend_len; i++) {
        if (g->pend[i].tick <= target) {
            feed(s, g, &g->pend[i]);
            upto = g->pend[i].tick;
            total++;
        } else {
            g->pend[keep++] = g->pend[i];
        }
    }
    g->pend_len = keep;
    all = keep > 0;

    while (!all && total < SESSION_REPLAY_STEP) {
        size_t n = 0;
        int err = store_read_merged(s->store, g->pos, buf, READ_MAX, &n);

        if (err == -ERANGE) {
            // The store dropped what the group had still to read: start again from its oldest.
            group_stop(g);
            items_clear(s->items, g->id);
            group_start(s, g);
            return true;
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i].tick > target) {
                g->pend[g->pend_len++] = buf[i];
                all = true;
                continue;
            }
            feed(s, g, &buf[i]);
            upto = buf[i].tick;
        }
        total += n;
        all = all || n < READ_MAX;
    }

    if (all) {
        upto = target;
        levels_before(s, g, target + 1);
        if (loss_due(g, UINT64_MAX)) {
            codec_advance(g->codec, target);
            losses_before(g, UINT64_MAX);
        }
    } else {
        // Records of the last tick read may not all be in yet.
        upto = upto > 0 ? upto - 1 : 0;
        s->catching_up = true;
    }

    if (!g->have_decoded || upto > g->decoded) {
        codec_advance(g->codec, upto);
        g->decoded = upto;
        g->have_decoded = true;
    }
    while ((it = codec_next(g->codec))) {
        items_add(s->items, g->id, it);
    }
    if (codec_error(g->codec) < 0 && !g->error) {
        g->error = codec_error(g->codec);
        snprintf(g->text, sizeof(g->text), "decoding failed (err: %d)", g->error);
    }

    return total > 0 || !all;
}


static void groups_publish(struct session *s)
{
    pthread_mutex_lock(&s->lock);
    s->dec_count = s->group_count;
    for (size_t i = 0; i < s->group_count; i++) {
        const struct group *g = &s->groups[i];
        struct session_decoder *d = &s->decs[i];

        memset(d, 0, sizeof(*d));
        d->group = g->id;
        snprintf(d->label, sizeof(d->label), "%s", g->label);
        snprintf(d->name, sizeof(d->name), "%s", g->specs[0].name);
        snprintf(d->args, sizeof(d->args), "%s", g->specs[0].args);
        d->stacked = g->spec_count - 1;
        d->hold = g->hold;
        d->decoded = g->decoded;
        d->error = g->error;
        snprintf(d->text, sizeof(d->text), "%s", g->text);
    }
    pthread_mutex_unlock(&s->lock);
}


static void status_publish(struct session *s)
{
    struct capture_status cst = {0};
    uint64_t overflow = 0;
    uint64_t decoded = UINT64_MAX;

    if (s->cap) {
        capture_status(s->cap, &cst);
        overflow = capture_overflow(s->cap);
    }
    for (size_t i = 0; i < s->group_count; i++) {
        if (s->groups[i].codec && s->groups[i].decoded < decoded) {
            decoded = s->groups[i].decoded;
        }
    }
    pthread_mutex_lock(&s->lock);
    s->st.backend = s->backend;
    s->st.state = !s->cap && !s->store ? SESSION_CLOSED :
                  s->reading ? SESSION_RUNNING : s->store ? SESSION_ENDED : SESSION_READY;
    if (s->cap) {
        s->st.capture = cst;
        s->st.overflow = overflow;
    }
    s->st.decoded = decoded == UINT64_MAX ? 0 : decoded;
    s->st.catching_up = s->catching_up;
    pthread_mutex_unlock(&s->lock);
}


static void event_add(struct session *s, uint64_t time, uint16_t code, uint16_t channel,
                      const char *text)
{
    struct session_event *e = NULL;

    pthread_mutex_lock(&s->lock);
    e = &s->events[s->event_count % EVENTS_MAX];
    s->event_count++;
    *e = (struct session_event){.seq = s->event_count, .time = time, .code = code,
                                .channel = channel};
    snprintf(e->text, sizeof(e->text), "%s", text);
    pthread_mutex_unlock(&s->lock);
}


static void on_record(void *ctx, const struct record *r)
{
    struct session *s = ctx;

    if (s->batch && s->reading) {
        store_batch_record(s->batch, r);
    }
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct session *s = ctx;

    if (s->batch && s->reading) {
        store_batch_heartbeat(s->batch, hb);
    }
}


static void on_loss(void *ctx, const struct loss *loss)
{
    struct session *s = ctx;

    if (s->batch && s->reading) {
        store_batch_loss(s->batch, loss);
    }
}


// The capture's events (capture/thcfile.h), as a line of text each.
static void on_event(void *ctx, uint16_t code, uint16_t channel, const uint8_t *data,
                     size_t len)
{
    struct session *s = ctx;
    char text[SESSION_TEXT_MAX];
    uint64_t time = s->cap ? capture_time(s->cap) : 0;

    switch (code) {
    case THC_EV_STOP:
        snprintf(text, sizeof(text), "stopped");
        break;
    case THC_EV_DATA_ERROR:
        snprintf(text, sizeof(text), "data channel %u: %.*s", channel,
                 len > 4 ? (int)(len - 4) : 0, len > 4 ? (const char *)&data[4] : "");
        break;
    case THC_EV_DATA_SILENT:
        snprintf(text, sizeof(text), "data channel %u silent for %llu ms", channel,
                 len >= 4 ? (unsigned long long)thc_get_le(data, 4) : 0ULL);
        break;
    case THC_EV_OVERFLOW:
        snprintf(text, sizeof(text), "data channel %u: %llu bytes and %llu messages dropped",
                 channel, len >= 8 ? (unsigned long long)thc_get_le(data, 8) : 0ULL,
                 len >= 12 ? (unsigned long long)thc_get_le(&data[8], 4) : 0ULL);
        break;
    case THC_EV_CONTROL_ERROR:
    case THC_EV_NOTE:
        snprintf(text, sizeof(text), "%.*s", (int)len, (const char *)data);
        break;
    default:
        snprintf(text, sizeof(text), "event 0x%04x", code);
        break;
    }
    event_add(s, time, code, channel, text);
}


// A group's label: its name= argument, or the decoder's name.
static void label_of(const char *name, const char *args, char *out, size_t size)
{
    const char *p = args;

    while (p && *p) {
        if (!strncmp(p, "name=", 5)) {
            size_t n = strcspn(p + 5, ",");

            snprintf(out, size, "%.*s", (int)n, p + 5);
            return;
        }
        p = strchr(p, ',');
        p = p ? p + 1 : NULL;
    }
    snprintf(out, size, "%s", name);
}
