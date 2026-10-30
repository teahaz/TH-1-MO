/*
 * The bus_replay Lua module (ui/docs/lua-api.md) over the process's session, and its
 * registration in every Lua state through a wrap of luaL_openlibs (-Wl,--wrap).
 *
 * CEL's page scripts have a require of their own that only finds modules as files, so the
 * wrap also answers package.searchpath and loadfile for this one module's name: both are
 * taken by CEL after luaL_openlibs, and nothing else changes.
 */

#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "capture/control.h"
#include "codec/codec.h"
#include "codec/item.h"
#include "common/capture_info.h"
#include "ui/cel/lua_session.h"
#include "ui/session/items.h"
#include "ui/session/session.h"
#include "ui/store/store.h"


#define CHANNELS_MAX 256
#define COLS_MAX 4096
#define ITEMS_MAX_DEFAULT 1000

static const uint32_t nice_rates[] = {
    1000000, 500000, 400000, 250000, 200000, 125000, 100000, 50000, 25000, 10000, 1000,
};

// The Lua source the wrap runs in each state, with luaopen_bus_replay as its argument.
static const char hook[] =
    "local open = ...\n"
    "local name, sentinel = 'bus_replay', '=bus_replay'\n"
    "local module\n"
    "local function get() module = module or open() return module end\n"
    "package.preload[name] = get\n"
    "local searchpath, loadfile_ = package.searchpath, loadfile\n"
    "package.searchpath = function(n, ...)\n"
    "    if n == name then return sentinel end\n"
    "    return searchpath(n, ...)\n"
    "end\n"
    "loadfile = function(path, ...)\n"
    "    if path == sentinel then return get end\n"
    "    return loadfile_(path, ...)\n"
    "end\n";

static struct session *session;

// An items callback's context: the table being filled, its label and the detail level.
struct fill {
    lua_State *L;
    const char *label;
    lua_Integer level;
    lua_Integer n;
};


void __real_luaL_openlibs(lua_State *L);
void __wrap_luaL_openlibs(lua_State *L);

static struct session *need(lua_State *L);
static int command(lua_State *L, int64_t id);
static int fail(lua_State *L, int err);
static size_t channels_arg(lua_State *L, int arg, uint16_t *out);
static void set_int(lua_State *L, const char *key, lua_Integer v);
static void set_str(lua_State *L, const char *key, const char *v);
static void set_bool(lua_State *L, const char *key, bool v);
static void push_item(lua_State *L, const struct codec_item *it, const char *label,
                      lua_Integer level);
static void fill_item(void *ctx, const struct codec_item *it);
static uint32_t group_of(const char *label);


void lua_session_set(struct session *s)
{
    session = s;
}


void __wrap_luaL_openlibs(lua_State *L)
{
    __real_luaL_openlibs(L);
    if (luaL_loadstring(L, hook) == LUA_OK) {
        lua_pushcfunction(L, luaopen_bus_replay);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}


static struct session *need(lua_State *L)
{
    if (!session) {
        luaL_error(L, "bus_replay: no session");
    }

    return session;
}


static int command(lua_State *L, int64_t id)
{
    if (id < 0) {
        return fail(L, (int)id);
    }
    lua_pushinteger(L, id);

    return 1;
}


static int fail(lua_State *L, int err)
{
    lua_pushnil(L);
    lua_pushstring(L, strerror(-err));

    return 2;
}


static size_t channels_arg(lua_State *L, int arg, uint16_t *out)
{
    size_t n = 0;

    luaL_checktype(L, arg, LUA_TTABLE);
    for (lua_Integer i = 1; n < CHANNELS_MAX; i++) {
        lua_Integer v = 0;
        int ok = 0;

        lua_geti(L, arg, i);
        v = lua_tointegerx(L, -1, &ok);
        lua_pop(L, 1);
        if (!ok || v < 0 || v > UINT16_MAX) {
            break;
        }
        out[n++] = (uint16_t)v;
    }

    return n;
}


static void set_int(lua_State *L, const char *key, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}


static void set_str(lua_State *L, const char *key, const char *v)
{
    lua_pushstring(L, v ? v : "");
    lua_setfield(L, -2, key);
}


static void set_bool(lua_State *L, const char *key, bool v)
{
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}


// br.open{backend=, path=, realtime=}
static int l_open(lua_State *L)
{
    static const char *const names[] = {"local", "dummy", "file", "remote", NULL};
    static const enum session_backend backends[] = {SESSION_LOCAL, SESSION_DUMMY,
                                                    SESSION_FILE, SESSION_REMOTE};
    struct session_open o = {.realtime = true};
    int b = 0;

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "backend");
    b = luaL_checkoption(L, -1, NULL, names);
    o.backend = backends[b];
    lua_getfield(L, 1, "path");
    o.path = lua_tostring(L, -1);
    lua_getfield(L, 1, "realtime");
    if (!lua_isnil(L, -1)) {
        o.realtime = lua_toboolean(L, -1);
    }

    return command(L, session_open(need(L), &o));
}


static int l_hello(lua_State *L)
{
    return command(L, session_hello(need(L)));
}


// br.start{rate=, c=, channels={...}}
static int l_start(lua_State *L)
{
    struct session_config cfg = {0};
    uint8_t set[CHANNELS_MAX / 8] = {0};

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "rate");
        cfg.rate = (uint32_t)luaL_optinteger(L, -1, 0);
        lua_getfield(L, 1, "c");
        cfg.c = (uint32_t)luaL_optinteger(L, -1, 0);
        lua_getfield(L, 1, "channels");
        if (lua_istable(L, -1)) {
            uint16_t ch[CHANNELS_MAX];
            size_t n = channels_arg(L, lua_gettop(L), ch);
            struct session_view *v = session_acquire(need(L));
            size_t size = v && v->hello ? ((size_t)v->hello->channels + 7) / 8 : 0;

            session_release(session, v);
            if (!size) {
                return fail(L, -ENODEV);
            }
            for (size_t i = 0; i < n; i++) {
                if (ch[i] / 8 < size) {
                    set[ch[i] / 8] |= (uint8_t)(1U << (ch[i] % 8));
                }
            }
            cfg.enabled = set;
            cfg.set_size = size;
        }
    }

    return command(L, session_start(need(L), &cfg));
}


static int l_stop(lua_State *L)
{
    return command(L, session_stop(need(L)));
}


static int l_record(lua_State *L)
{
    return command(L, session_record(need(L), luaL_optstring(L, 1, NULL)));
}


static int l_close(lua_State *L)
{
    return command(L, session_close(need(L)));
}


// br.decoder_add(name, args, on, hold)
static int l_decoder_add(lua_State *L)
{
    return command(L, session_decoder_add(need(L), luaL_checkstring(L, 1),
                                          luaL_optstring(L, 2, ""), luaL_optstring(L, 3, NULL),
                                          luaL_optnumber(L, 4, 0)));
}


static int l_decoder_remove(lua_State *L)
{
    return command(L, session_decoder_remove(need(L), luaL_checkstring(L, 1)));
}


static int l_status(lua_State *L)
{
    static const char *const states[] = {"closed", "ready", "running", "ended"};
    static const char *const backends[] = {"none", "local", "dummy", "file", "remote"};
    struct session_status st;
    struct session_view *v = NULL;

    session_status(need(L), &st);
    lua_createtable(L, 0, 16);
    set_str(L, "state", states[st.state]);
    set_str(L, "backend", backends[st.backend]);
    set_bool(L, "busy", st.busy);
    set_int(L, "done", (lua_Integer)st.done);
    set_bool(L, "ok", st.result == 0);
    if (st.result < 0) {
        set_str(L, "error", st.text[0] ? st.text : strerror(-st.result));
    }
    set_int(L, "gen", (lua_Integer)st.gen);
    set_int(L, "events", (lua_Integer)st.events);
    set_int(L, "decoded", (lua_Integer)st.decoded);
    set_bool(L, "catching_up", st.catching_up);

    lua_createtable(L, 0, 14);
    set_bool(L, "running", st.capture.running);
    set_int(L, "now", (lua_Integer)st.capture.now);
    set_int(L, "records", (lua_Integer)st.capture.total.records);
    set_int(L, "heartbeats", (lua_Integer)st.capture.total.heartbeats);
    set_int(L, "bytes", (lua_Integer)st.capture.total.bytes);
    set_int(L, "crc", (lua_Integer)st.capture.total.crc_errors);
    set_int(L, "gaps", (lua_Integer)st.capture.total.gap_errors);
    set_int(L, "resyncs", (lua_Integer)st.capture.total.resyncs);
    set_int(L, "mismatches", (lua_Integer)st.capture.total.mismatches);
    set_int(L, "excused", (lua_Integer)st.capture.total.excused);
    set_int(L, "losses", (lua_Integer)st.capture.losses);
    set_int(L, "overflow", (lua_Integer)st.overflow);
    lua_setfield(L, -2, "capture");

    v = session_acquire(session);
    if (v && v->store) {
        struct store_stats ss;

        store_stats(v->store, &ss);
        lua_createtable(L, 0, 8);
        set_int(L, "oldest", (lua_Integer)ss.oldest);
        set_int(L, "watermark", ss.have_watermark ? (lua_Integer)ss.watermark : -1);
        set_int(L, "newest", (lua_Integer)ss.newest);
        set_int(L, "records", (lua_Integer)ss.records);
        set_int(L, "total", (lua_Integer)ss.total);
        set_int(L, "dropped", (lua_Integer)ss.dropped);
        set_int(L, "bytes", (lua_Integer)ss.bytes);
        set_int(L, "limit", (lua_Integer)ss.limit);
        lua_setfield(L, -2, "store");
    }
    session_release(session, v);

    return 1;
}


static int l_events(lua_State *L)
{
    struct session_event ev[64];
    uint64_t after = (uint64_t)luaL_optinteger(L, 1, 0);
    size_t n = session_events(need(L), after, ev, 64);

    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 5);
        set_int(L, "seq", (lua_Integer)ev[i].seq);
        lua_pushnumber(L, (lua_Number)ev[i].time / 1e9);
        lua_setfield(L, -2, "time");
        set_int(L, "code", ev[i].code);
        set_int(L, "channel", ev[i].channel);
        set_str(L, "text", ev[i].text);
        lua_seti(L, -2, (lua_Integer)i + 1);
    }

    return 1;
}


// The hello of the view: the device, its registers and what they allow.
static int l_device(lua_State *L)
{
    struct session_view *v = session_acquire(need(L));
    const struct hello *h = v ? v->hello : NULL;
    const struct reg *r = NULL;
    char buf[2 * STR_MAX + 1];
    uint32_t base = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;

    if (!h) {
        session_release(session, v);
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 14);
    set_str(L, "model", h->model);
    set_str(L, "firmware", h->firmware);
    snprintf(buf, sizeof(buf), "%u.%u", h->version_major, h->version_minor);
    set_str(L, "version", buf);
    for (size_t i = 0; i < h->uid_len; i++) {
        snprintf(&buf[2 * i], 3, "%02x", h->uid[i]);
    }
    buf[2 * h->uid_len] = '\0';
    set_str(L, "uid", buf);
    set_int(L, "buffer", h->buffer);
    set_int(L, "channels", h->channels);
    set_int(L, "data_channels", h->data_channels);
    lua_createtable(L, h->channels, 1);
    for (unsigned int ch = 0; ch < h->channels; ch++) {
        lua_pushstring(L, h->names[ch]);
        lua_seti(L, -2, ch);
    }
    lua_setfield(L, -2, "names");

    r = hello_reg(h, REG_SAMPLE_RATE);
    if (r) {
        set_int(L, "rate", reg_u32(r));
        lua_newtable(L);
        if (reg_divider(r, &base, &lo, &hi) == 0) {
            lua_Integer k = 1;

            for (size_t i = 0; i < sizeof(nice_rates) / sizeof(nice_rates[0]); i++) {
                uint32_t d = nice_rates[i] ? base / nice_rates[i] : 0;

                if (d && base % nice_rates[i] == 0 && d >= lo && d <= hi) {
                    lua_pushinteger(L, nice_rates[i]);
                    lua_seti(L, -2, k++);
                }
            }
        }
        lua_setfield(L, -2, "rates");
    }
    r = hello_reg(h, REG_C);
    if (r) {
        set_int(L, "c", reg_u32(r));
        if (reg_range(r, &lo, &hi) == 0) {
            set_int(L, "c_min", lo);
            set_int(L, "c_max", hi);
        }
    }
    r = hello_reg(h, REG_ENABLED);
    if (r) {
        lua_Integer k = 1;

        lua_newtable(L);
        for (unsigned int ch = 0; ch < h->channels && ch / 8 < r->size; ch++) {
            if (channel_set_has(r->value, ch)) {
                lua_pushinteger(L, ch);
                lua_seti(L, -2, k++);
            }
        }
        lua_setfield(L, -2, "enabled");
    }
    session_release(session, v);

    return 1;
}


static int l_capture(lua_State *L)
{
    struct session_view *v = session_acquire(need(L));
    const struct capture_info *info = v ? v->info : NULL;

    if (!info) {
        session_release(session, v);
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 5);
    set_int(L, "gen", (lua_Integer)v->gen);
    set_int(L, "rate", info->rate);
    set_int(L, "c", info->c);
    lua_createtable(L, info->n, 0);
    for (uint16_t i = 0; i < info->n; i++) {
        lua_pushinteger(L, info->enabled[i]);
        lua_seti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "channels");
    lua_createtable(L, info->channels, 1);
    for (unsigned int ch = 0; ch < info->channels; ch++) {
        lua_pushstring(L, info->names[ch]);
        lua_seti(L, -2, ch);
    }
    lua_setfield(L, -2, "names");
    session_release(session, v);

    return 1;
}


static int level_value(uint8_t level)
{
    return level == STORE_UNKNOWN ? -1 : level;
}


// The lanes of a frame: per channel, parallel lists over the columns.
static int columns(lua_State *L, const uint16_t *ch, size_t count, const uint64_t *edges,
                   size_t cols)
{
    struct session_view *v = session_acquire(need(L));
    struct store_column *out = NULL;
    struct store_stats ss;
    int err = 0;

    if (!v || !v->store) {
        session_release(session, v);
        return fail(L, -ENODATA);
    }
    out = malloc((count * cols > 0 ? count * cols : 1) * sizeof(*out));
    store_stats(v->store, &ss);
    err = out ? store_columns(v->store, ch, count, edges, cols, out) : -ENOMEM;
    if (err < 0) {
        free(out);
        session_release(session, v);
        return fail(L, err);
    }
    lua_createtable(L, (int)count, 2);
    set_int(L, "gen", (lua_Integer)v->gen);
    set_int(L, "watermark", ss.have_watermark ? (lua_Integer)ss.watermark : -1);
    for (size_t k = 0; k < count; k++) {
        static const char *const keys[] = {"first", "last", "changes", "flags"};

        lua_createtable(L, 0, 4);
        for (int f = 0; f < 4; f++) {
            lua_createtable(L, (int)cols, 0);
            for (size_t x = 0; x < cols; x++) {
                const struct store_column *c = &out[k * cols + x];
                lua_Integer val = f == 0 ? level_value(c->start_level) :
                                  f == 1 ? level_value(c->end_level) :
                                  f == 2 ? (lua_Integer)c->changes : c->flags;

                lua_pushinteger(L, val);
                lua_seti(L, -2, (lua_Integer)x + 1);
            }
            lua_setfield(L, -2, keys[f]);
        }
        lua_seti(L, -2, (lua_Integer)k + 1);
    }
    free(out);
    session_release(session, v);

    return 1;
}


// br.columns(channels, edges)
static int l_columns(lua_State *L)
{
    uint16_t ch[CHANNELS_MAX];
    size_t count = channels_arg(L, 1, ch);
    uint64_t *edges = NULL;
    size_t n = 0;
    int ret = 0;

    luaL_checktype(L, 2, LUA_TTABLE);
    n = (size_t)luaL_len(L, 2);
    if (n < 2 || n > COLS_MAX + 1) {
        return fail(L, -EINVAL);
    }
    edges = malloc(n * sizeof(*edges));
    if (!edges) {
        return fail(L, -ENOMEM);
    }
    for (size_t i = 0; i < n; i++) {
        lua_geti(L, 2, (lua_Integer)i + 1);
        edges[i] = (uint64_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    ret = columns(L, ch, count, edges, n - 1);
    free(edges);

    return ret;
}


// br.span(channels, from, to, cols): cols equal columns over ticks from to to - 1.
static int l_span(lua_State *L)
{
    uint16_t ch[CHANNELS_MAX];
    size_t count = channels_arg(L, 1, ch);
    uint64_t from = (uint64_t)luaL_checkinteger(L, 2);
    uint64_t to = (uint64_t)luaL_checkinteger(L, 3);
    lua_Integer cols = luaL_checkinteger(L, 4);
    uint64_t *edges = NULL;
    int ret = 0;

    if (cols < 1 || cols > COLS_MAX || to < from) {
        return fail(L, -EINVAL);
    }
    edges = malloc(((size_t)cols + 1) * sizeof(*edges));
    if (!edges) {
        return fail(L, -ENOMEM);
    }
    for (lua_Integer x = 0; x <= cols; x++) {
        edges[x] = from + (to - from) * (uint64_t)x / (uint64_t)cols;
    }
    ret = columns(L, ch, count, edges, (size_t)cols);
    free(edges);

    return ret;
}


static int l_level(lua_State *L)
{
    lua_Integer ch = luaL_checkinteger(L, 1);
    lua_Integer tick = luaL_checkinteger(L, 2);
    struct session_view *v = session_acquire(need(L));
    int level = v && v->store ? store_level(v->store, (uint16_t)ch, (uint64_t)tick) : -ENODATA;

    session_release(session, v);
    if (level < 0) {
        return fail(L, level);
    }
    lua_pushinteger(L, level_value((uint8_t)level));

    return 1;
}


static int next_prev(lua_State *L, bool next)
{
    uint16_t ch[CHANNELS_MAX];
    size_t count = channels_arg(L, 1, ch);
    uint64_t tick = (uint64_t)luaL_checkinteger(L, 2);
    struct session_view *v = session_acquire(need(L));
    struct record r;
    int err = !v || !v->store ? -ENODATA :
              next ? store_next(v->store, ch, count, tick, &r) :
                     store_prev(v->store, ch, count, tick, &r);

    session_release(session, v);
    if (err < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)r.tick);
    lua_pushinteger(L, r.channel);
    lua_pushinteger(L, r.level);

    return 3;
}


static int l_next(lua_State *L)
{
    return next_prev(L, true);
}


static int l_prev(lua_State *L)
{
    return next_prev(L, false);
}


// br.records(channel, from, to, max): {ticks=, levels=}, more
static int l_records(lua_State *L)
{
    lua_Integer ch = luaL_checkinteger(L, 1);
    uint64_t from = (uint64_t)luaL_checkinteger(L, 2);
    uint64_t to = (uint64_t)luaL_checkinteger(L, 3);
    lua_Integer max = luaL_optinteger(L, 4, 10000);
    struct session_view *v = session_acquire(need(L));
    struct record *out = NULL;
    size_t n = 0;
    bool more = false;
    int err = 0;

    if (max < 1 || max > 1000000) {
        session_release(session, v);
        return fail(L, -EINVAL);
    }
    out = malloc((size_t)max * sizeof(*out));
    err = !out ? -ENOMEM : !v || !v->store ? -ENODATA :
          store_records(v->store, (uint16_t)ch, from, to, out, (size_t)max, &n, &more);
    session_release(session, v);
    if (err < 0) {
        free(out);
        return fail(L, err);
    }
    lua_createtable(L, 0, 2);
    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)out[i].tick);
        lua_seti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "ticks");
    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_pushinteger(L, out[i].level);
        lua_seti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "levels");
    lua_pushboolean(L, more);
    free(out);

    return 2;
}


// br.losses(from, to, max)
static int l_losses(lua_State *L)
{
    uint64_t from = (uint64_t)luaL_checkinteger(L, 1);
    uint64_t to = (uint64_t)luaL_checkinteger(L, 2);
    lua_Integer max = luaL_optinteger(L, 3, 256);
    struct session_view *v = session_acquire(need(L));
    struct store_loss *out = NULL;
    uint32_t *lost = NULL;
    size_t n = 0;
    int err = 0;

    if (!v || !v->store) {
        session_release(session, v);
        return fail(L, -ENODATA);
    }
    if (max < 1 || max > 65536) {
        session_release(session, v);
        return fail(L, -EINVAL);
    }
    out = malloc((size_t)max * sizeof(*out));
    lost = malloc((size_t)max * (v->info->n ? v->info->n : 1) * sizeof(*lost));
    err = out && lost ? store_losses(v->store, from, to, out, lost, (size_t)max, &n) : -ENOMEM;
    if (err < 0) {
        free(out);
        free(lost);
        session_release(session, v);
        return fail(L, err);
    }
    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 4);
        set_int(L, "start", (lua_Integer)out[i].start);
        set_int(L, "stop", (lua_Integer)out[i].end);
        set_int(L, "lost", (lua_Integer)out[i].lost);
        lua_newtable(L);
        for (uint16_t k = 0; k < v->info->n; k++) {
            if (lost[i * v->info->n + k]) {
                lua_pushinteger(L, lost[i * v->info->n + k]);
                lua_seti(L, -2, v->info->enabled[k]);
            }
        }
        lua_setfield(L, -2, "channels");
        lua_seti(L, -2, (lua_Integer)i + 1);
    }
    free(out);
    free(lost);
    session_release(session, v);

    return 1;
}


/*
 * br.protocols(): the decoders that read lines (not stacked on another), with their
 * arguments as forms need them: { {name, summary, params = { {name, help, type, required,
 * default}, ... }}, ... }, type "line", "uint", "real" or "text".
 */
static int l_protocols(lua_State *L)
{
    static const char *const types[] = {"text", "line", "uint", "real"};
    int n = 0;

    lua_newtable(L);
    for (size_t i = 0; codec_decoders[i]; i++) {
        const struct codec_decoder *def = codec_decoders[i];
        int k = 0;

        if (def->input) {
            continue;
        }
        lua_createtable(L, 0, 3);
        set_str(L, "name", def->name);
        set_str(L, "summary", def->summary);
        lua_newtable(L);
        for (const struct codec_param *p = def->params; p && p->name; p++) {
            lua_createtable(L, 0, 5);
            set_str(L, "name", p->name);
            set_str(L, "help", p->help);
            set_str(L, "type", p->type <= CODEC_PARAM_REAL ? types[p->type] : "text");
            set_bool(L, "required", p->flags & CODEC_PARAM_REQUIRED);
            if (p->def) {
                set_str(L, "default", p->def);
            }
            lua_seti(L, -2, ++k);
        }
        lua_setfield(L, -2, "params");
        lua_seti(L, -2, ++n);
    }

    return 1;
}


static int l_decoders(lua_State *L)
{
    struct session_decoder d[SESSION_DECODERS_MAX];
    size_t n = session_decoders(need(L), d, SESSION_DECODERS_MAX);

    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 8);
        set_str(L, "label", d[i].label);
        set_str(L, "name", d[i].name);
        set_str(L, "args", d[i].args);
        set_int(L, "stacked", (lua_Integer)d[i].stacked);
        lua_pushnumber(L, d[i].hold);
        lua_setfield(L, -2, "hold");
        set_int(L, "decoded", (lua_Integer)d[i].decoded);
        if (d[i].error < 0) {
            set_str(L, "error", d[i].text[0] ? d[i].text : strerror(-d[i].error));
        }
        lua_seti(L, -2, (lua_Integer)i + 1);
    }

    return 1;
}


static uint32_t group_of(const char *label)
{
    struct session_decoder d[SESSION_DECODERS_MAX];
    size_t n = session_decoders(session, d, SESSION_DECODERS_MAX);

    for (size_t i = 0; i < n; i++) {
        if (!strcmp(d[i].label, label)) {
            return d[i].group;
        }
    }

    return 0;
}


static void push_flags(lua_State *L, const struct codec_kind *kind, uint32_t flags, bool words)
{
    lua_Integer k = 1;

    lua_newtable(L);
    for (unsigned int bit = 0; bit < (words ? 8U : 32U); bit++) {
        const char *name = NULL;

        if (!(flags & (1U << bit))) {
            continue;
        }
        name = words ? codec_word_flag_name(kind, bit) : codec_flag_name(kind, bit);
        if (name) {
            lua_pushstring(L, name);
            lua_seti(L, -2, k++);
        }
    }
}


/*
 * An item as a table, children to detail level. Its decoder is named by the group's label
 * (its own decoder may be gone: items outlive a group's codec session).
 */
static void push_item(lua_State *L, const struct codec_item *it, const char *label,
                      lua_Integer level)
{
    const struct codec_kind *kind = it->kind;
    lua_Integer k = 1;

    lua_createtable(L, 0, 12);
    set_int(L, "id", (lua_Integer)it->id);
    set_str(L, "kind", kind->name);
    set_str(L, "decoder", label);
    set_int(L, "start", (lua_Integer)it->start);
    set_int(L, "stop", (lua_Integer)it->end);
    set_bool(L, "text", kind->flags & CODEC_KIND_TEXT);
    push_flags(L, kind, it->flags, false);
    lua_setfield(L, -2, "flags");

    lua_createtable(L, 0, kind->field_count);
    for (uint8_t f = 0; f < kind->field_count; f++) {
        const struct codec_field_desc *fd = &kind->fields[f];
        const struct codec_value *val = &it->values[f];

        if (!val->set) {
            continue;
        }
        switch (fd->type) {
        case CODEC_UINT:
            lua_pushinteger(L, (lua_Integer)val->u);
            break;
        case CODEC_INT:
            lua_pushinteger(L, (lua_Integer)val->i);
            break;
        case CODEC_REAL:
            lua_pushnumber(L, val->r);
            break;
        case CODEC_TEXT:
            lua_pushstring(L, val->s ? val->s : "");
            break;
        default:
            lua_pushboolean(L, val->b);
            break;
        }
        lua_setfield(L, -2, fd->name);
    }
    lua_setfield(L, -2, "fields");

    lua_createtable(L, kind->field_count, 0);
    for (uint8_t f = 0; f < kind->field_count; f++) {
        lua_pushstring(L, kind->fields[f].name);
        lua_seti(L, -2, f + 1);
    }
    lua_setfield(L, -2, "order");
    lua_newtable(L);
    for (uint8_t f = 0; f < kind->field_count; f++) {
        if (kind->fields[f].flags & CODEC_FIELD_HEADER) {
            lua_pushstring(L, kind->fields[f].name);
            lua_seti(L, -2, k++);
        }
    }
    lua_setfield(L, -2, "header");

    lua_createtable(L, (int)it->word_count, 0);
    for (size_t w = 0; w < it->word_count; w++) {
        const struct codec_word *word = &it->words[w];

        lua_createtable(L, 0, 6);
        set_int(L, "start", (lua_Integer)word->start);
        set_int(L, "stop", (lua_Integer)word->end);
        set_int(L, "value", (lua_Integer)word->value);
        set_int(L, "bits", word->bits);
        if (word->lane < kind->lane_count) {
            set_str(L, "lane", kind->lanes[word->lane]);
        }
        push_flags(L, kind, word->flags, true);
        lua_setfield(L, -2, "flags");
        lua_seti(L, -2, (lua_Integer)w + 1);
    }
    lua_setfield(L, -2, "words");

    lua_newtable(L);
    k = 1;
    for (const struct codec_item *ch = it->child; ch; ch = ch->next) {
        if (ch->detail <= level) {
            push_item(L, ch, label, level);
            lua_seti(L, -2, k++);
        }
    }
    lua_setfield(L, -2, "children");
}


static void fill_item(void *ctx, const struct codec_item *it)
{
    struct fill *f = ctx;

    push_item(f->L, it, f->label, f->level);
    lua_seti(f->L, -2, ++f->n);
}


// The items of a group, by span (items), newest (last) or id (item).
static int items_call(lua_State *L, int how)
{
    const char *label = luaL_checkstring(L, 1);
    struct fill f = {.L = L, .label = label};
    uint64_t a = 0;
    uint64_t b = 0;
    size_t max = 0;
    struct session_view *v = NULL;
    uint32_t group = 0;

    if (how == 0) {
        a = (uint64_t)luaL_checkinteger(L, 2);
        b = (uint64_t)luaL_checkinteger(L, 3);
        max = (size_t)luaL_optinteger(L, 4, ITEMS_MAX_DEFAULT);
        f.level = luaL_optinteger(L, 5, 0);
    } else {
        a = (uint64_t)(how == 1 ? luaL_optinteger(L, 2, 1) : luaL_checkinteger(L, 2));
        f.level = luaL_optinteger(L, 3, 0);
    }
    v = session_acquire(need(L));
    group = group_of(label);
    if (!group) {
        session_release(session, v);
        return fail(L, -ENOENT);
    }
    lua_newtable(L);
    if (v && v->items) {
        if (how == 0) {
            items_span(v->items, group, a, b, max, fill_item, &f);
        } else if (how == 1) {
            items_last(v->items, group, (size_t)a, fill_item, &f);
        } else {
            items_find(v->items, group, a, fill_item, &f);
        }
    }
    session_release(session, v);
    if (how == 2) {
        lua_geti(L, -1, 1);
    }

    return 1;
}


static int l_items(lua_State *L)
{
    return items_call(L, 0);
}


static int l_last(lua_State *L)
{
    return items_call(L, 1);
}


static int l_item(lua_State *L)
{
    return items_call(L, 2);
}


// br.clock(): seconds on a monotonic clock, for the UI's timing (the page has no other).
static int l_clock(lua_State *L)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushnumber(L, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);

    return 1;
}


int luaopen_bus_replay(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"open", l_open},
        {"hello", l_hello},
        {"start", l_start},
        {"stop", l_stop},
        {"record", l_record},
        {"close", l_close},
        {"decoder_add", l_decoder_add},
        {"decoder_remove", l_decoder_remove},
        {"status", l_status},
        {"events", l_events},
        {"device", l_device},
        {"capture", l_capture},
        {"columns", l_columns},
        {"span", l_span},
        {"level", l_level},
        {"next", l_next},
        {"prev", l_prev},
        {"records", l_records},
        {"losses", l_losses},
        {"decoders", l_decoders},
        {"protocols", l_protocols},
        {"items", l_items},
        {"last", l_last},
        {"item", l_item},
        {"clock", l_clock},
        {NULL, NULL},
    };

    luaL_newlib(L, fns);

    return 1;
}
