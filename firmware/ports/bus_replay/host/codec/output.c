#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "codec/item.h"
#include "codec/output.h"


// Words start below the header's fields: past the time and two spaces.
#define INDENT 13
#define HEX_PER_LINE 16
#define TEXT_PER_LINE 120
#define LINE_MAX 1024


struct line {
    char s[LINE_MAX];
    size_t len;
};


static void put(struct line *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pad(struct line *l, size_t to);
static void flush(FILE *out, struct line *l);
static void header(struct line *l, const struct codec_item *item, size_t first_width);
static void flags(struct line *l, const struct codec_item *item);
static void value_text(struct line *l, const struct codec_field_desc *f,
                       const struct codec_value *v);
static void text_item(FILE *out, const struct codec_item *item, const struct output_options *opt,
                      unsigned int depth);
static void pretty_item(FILE *out, const struct codec_item *item,
                        const struct output_options *opt);
static void words_runs(FILE *out, struct line *l, const struct codec_item *item,
                       const struct output_options *opt, size_t indent);
static void words_turns(FILE *out, const struct codec_item *item,
                        const struct output_options *opt, size_t indent);
static void words_side(FILE *out, const struct codec_item *item, size_t indent);
static void words_hex(FILE *out, struct line *l, const struct codec_word *w, size_t count,
                      size_t indent);
static void words_string(FILE *out, struct line *l, const struct codec_word *w, size_t count,
                         size_t indent);
static const char *lane_mark(const struct codec_kind *kind, uint8_t lane);
static void json_item(FILE *out, const struct codec_item *item, const struct output_options *opt);
static void json_str(FILE *out, const char *s);
static void json_flags(FILE *out, const struct codec_kind *kind, uint32_t flags, bool word);


/*
 * One top-level item as text: a header line (time, decoder, header fields, flags), its
 * words, then its children up to the detail level, indented.
 */
int output_text(FILE *out, const struct codec_item *item, const struct output_options *opt)
{
    struct line l = {0};

    if (opt->pretty) {
        pretty_item(out, item, opt);
        return ferror(out) ? -EIO : 0;
    }
    put(&l, "%10.6f  %-*s  ", (double)item->start / opt->rate, (int)opt->name_width,
        item->dec->name);
    header(&l, item, 0);
    flush(out, &l);
    text_item(out, item, opt, 0);

    return ferror(out) ? -EIO : 0;
}


// One top-level item as a JSON object on one line, with its whole tree up to the level.
int output_jsonl(FILE *out, const struct codec_item *item, const struct output_options *opt)
{
    json_item(out, item, opt);
    fputc('\n', out);

    return ferror(out) ? -EIO : 0;
}


static void put(struct line *l, const char *fmt, ...)
{
    va_list ap;
    int n = 0;

    if (l->len >= sizeof(l->s) - 1) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(&l->s[l->len], sizeof(l->s) - l->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        l->len += (size_t)n;
        if (l->len > sizeof(l->s) - 1) {
            l->len = sizeof(l->s) - 1;
        }
    }
}


static void pad(struct line *l, size_t to)
{
    while (l->len < to && l->len < sizeof(l->s) - 1) {
        l->s[l->len++] = ' ';
    }
    l->s[l->len] = '\0';
}


// Writes the line without trailing spaces, and empties it.
static void flush(FILE *out, struct line *l)
{
    while (l->len && l->s[l->len - 1] == ' ') {
        l->len--;
    }
    fprintf(out, "%.*s\n", (int)l->len, l->s);
    l->len = 0;
    l->s[0] = '\0';
}


/*
 * The item's header fields, each at least its width, then its flags. A first_width other
 * than 0 is the first field's width instead of its own.
 */
static void header(struct line *l, const struct codec_item *item, size_t first_width)
{
    const struct codec_kind *kind = item->kind;
    bool first = true;

    for (unsigned int f = 0; f < kind->field_count; f++) {
        const struct codec_field_desc *fd = &kind->fields[f];
        size_t at = 0;

        if (!(fd->flags & CODEC_FIELD_HEADER) || !item->values[f].set) {
            continue;
        }
        if (!first) {
            put(l, "  ");
        }
        at = l->len;
        value_text(l, fd, &item->values[f]);
        if (fd->unit) {
            put(l, " %s", fd->unit);
        }
        pad(l, at + (first && first_width ? first_width : fd->width));
        first = false;
    }
    if (item->flags) {
        put(l, "  ");
        flags(l, item);
    }
}


// The item's flags by name, in brackets.
static void flags(struct line *l, const struct codec_item *item)
{
    bool any = false;

    put(l, "[");
    for (unsigned int b = 0; b < 32; b++) {
        const char *name = (item->flags >> b) & 1 ? codec_flag_name(item->kind, b) : NULL;

        if (name) {
            put(l, "%s%s", any ? ", " : "", name);
            any = true;
        }
    }
    put(l, "]");
}


static void value_text(struct line *l, const struct codec_field_desc *f,
                       const struct codec_value *v)
{
    switch (f->type) {
    case CODEC_UINT:
        if (f->flags & CODEC_FIELD_HEX) {
            put(l, "0x%0*" PRIx64, f->digits, v->u);
        } else {
            put(l, "%" PRIu64, v->u);
        }
        break;
    case CODEC_INT:
        put(l, "%" PRId64, v->i);
        break;
    case CODEC_REAL:
        if (f->digits) {
            put(l, "%.*f", f->digits, v->r);
        } else {
            put(l, "%g", v->r);
        }
        break;
    case CODEC_TEXT:
        put(l, "%s", v->s ? v->s : "");
        break;
    case CODEC_BOOL:
        put(l, "%s", v->b ? "yes" : "no");
        break;
    default:
        break;
    }
}


static void text_item(FILE *out, const struct codec_item *item, const struct output_options *opt,
                      unsigned int depth)
{
    size_t indent = INDENT + 2 * depth;

    if (item->word_count) {
        if (item->kind->lane_mode == CODEC_LANES_SIDE) {
            words_side(out, item, indent);
        } else {
            words_turns(out, item, opt, indent);
        }
    }

    for (const struct codec_item *c = item->child; c; c = c->next) {
        struct line l = {0};

        if (c->detail > opt->level) {
            continue;
        }
        pad(&l, indent + 2);
        // A child from another decoder is its reading of this item.
        put(&l, "%s ", c->dec != item->dec ? "=" : "-");
        header(&l, c, 0);
        flush(out, &l);
        text_item(out, c, opt, depth + 2);
    }
}


/*
 * One top-level item, pretty: the readings other decoders made of it, a line each at its
 * own time, with the reading's header fields (a chip's starting with its name). With none,
 * the item itself on one line: a kind read as text (UART) as its words, any other as its
 * header line without words.
 */
static void pretty_item(FILE *out, const struct codec_item *item,
                        const struct output_options *opt)
{
    struct line l = {0};
    bool read = false;

    for (const struct codec_item *c = item->child; c; c = c->next) {
        if (c->dec == item->dec) {
            continue;
        }
        // The reading's first field (a chip's name) takes the label column.
        put(&l, "%10.6f  ", (double)c->start / opt->rate);
        header(&l, c, opt->name_width);
        flush(out, &l);
        read = true;
    }
    if (read) {
        return;
    }

    put(&l, "%10.6f  %-*s  ", (double)item->start / opt->rate, (int)opt->name_width,
        item->dec->name);
    if ((item->kind->flags & CODEC_KIND_TEXT) && item->kind->lane_mode == CODEC_LANES_TURNS &&
        item->word_count) {
        if (item->flags) {
            flags(&l, item);
            put(&l, " ");
        }
        words_runs(out, &l, item, opt, l.len);
    } else {
        header(&l, item, 0);
        flush(out, &l);
    }
}


/*
 * Lanes taking turns: a line per group, a group being the words of one lane until the lane
 * changes or a word starts a new group.
 */
static void words_turns(FILE *out, const struct codec_item *item,
                        const struct output_options *opt, size_t indent)
{
    struct line l = {0};

    pad(&l, indent);
    words_runs(out, &l, item, opt, indent);
}


/*
 * The groups of words_turns(): the first goes on in l, the others start on lines of their
 * own at indent.
 */
static void words_runs(FILE *out, struct line *l, const struct codec_item *item,
                       const struct output_options *opt, size_t indent)
{
    const struct codec_word *w = item->words;
    bool text = (item->kind->flags & CODEC_KIND_TEXT) && !opt->hex;
    size_t i = 0;

    while (i < item->word_count) {
        size_t n = 1;

        while (i + n < item->word_count && w[i + n].lane == w[i].lane &&
               !(w[i + n].flags & CODEC_WORD_BREAK)) {
            n++;
        }
        if (i) {
            pad(l, indent);
        }
        put(l, "%s ", lane_mark(item->kind, w[i].lane));
        if (text) {
            words_string(out, l, &w[i], n, l->len);
        } else {
            words_hex(out, l, &w[i], n, l->len);
        }
        i += n;
    }
}


// Lanes side by side: a line per lane, named.
static void words_side(FILE *out, const struct codec_item *item, size_t indent)
{
    const struct codec_kind *kind = item->kind;
    struct codec_word *lane = malloc(item->word_count * sizeof(*lane));
    size_t name_w = 0;

    if (!lane) {
        return;
    }
    for (unsigned int k = 0; k < kind->lane_count; k++) {
        size_t n = strlen(kind->lanes[k]);

        name_w = n > name_w ? n : name_w;
    }
    for (unsigned int k = 0; k < kind->lane_count; k++) {
        struct line l = {0};
        size_t n = 0;

        for (size_t i = 0; i < item->word_count; i++) {
            if (item->words[i].lane == k) {
                lane[n++] = item->words[i];
            }
        }
        if (!n) {
            continue;
        }
        pad(&l, indent);
        put(&l, "%-*s  ", (int)name_w, kind->lanes[k]);
        words_hex(out, &l, lane, n, l.len);
    }
    free(lane);
}


static void words_hex(FILE *out, struct line *l, const struct codec_word *w, size_t count,
                      size_t indent)
{
    for (size_t i = 0; i < count; i++) {
        if (i && i % HEX_PER_LINE == 0) {
            flush(out, l);
            pad(l, indent);
        }
        put(l, "%s%0*" PRIX64, i % HEX_PER_LINE ? " " : "", (w[i].bits + 3) / 4, w[i].value);
    }
    flush(out, l);
}


/*
 * Words as a quoted string: printable bytes as they are, the rest escaped. A line feed ends
 * the line; the string goes on below.
 */
static void words_string(FILE *out, struct line *l, const struct codec_word *w, size_t count,
                         size_t indent)
{
    size_t col = 0;

    put(l, "\"");
    for (size_t i = 0; i < count; i++) {
        uint64_t c = w[i].value;

        if (c == '"' || c == '\\') {
            put(l, "\\%c", (char)c);
        } else if (c == '\n') {
            put(l, "\\n");
        } else if (c == '\r') {
            put(l, "\\r");
        } else if (c == '\t') {
            put(l, "\\t");
        } else if (c >= 0x20 && c < 0x7F) {
            put(l, "%c", (char)c);
        } else {
            put(l, "\\x%02" PRIX64, c);
        }
        col++;
        // A long line breaks anywhere but before its line ending.
        if (i + 1 < count &&
            (c == '\n' || (col >= TEXT_PER_LINE && w[i + 1].value != '\r' &&
                            w[i + 1].value != '\n'))) {
            put(l, "\"");
            flush(out, l);
            pad(l, indent);
            put(l, "\"");
            col = 0;
        }
    }
    put(l, "\"");
    flush(out, l);
}


// > and < for the first two lanes taking turns; the lane's name past them.
static const char *lane_mark(const struct codec_kind *kind, uint8_t lane)
{
    if (kind->lane_count <= 2) {
        return lane == 0 ? ">" : "<";
    }

    return lane < kind->lane_count ? kind->lanes[lane] : "?";
}


static void json_item(FILE *out, const struct codec_item *item, const struct output_options *opt)
{
    const struct codec_kind *kind = item->kind;
    bool first = true;

    fprintf(out, "{\"decoder\":");
    json_str(out, item->dec->name);
    fprintf(out, ",\"kind\":");
    json_str(out, kind->name);
    fprintf(out,
            ",\"id\":%" PRIu64 ",\"start\":%" PRIu64 ",\"end\":%" PRIu64
            ",\"start_s\":%.9g,\"end_s\":%.9g",
            item->id, item->start, item->end, (double)item->start / opt->rate,
            (double)item->end / opt->rate);
    if (item->flags) {
        fprintf(out, ",\"flags\":");
        json_flags(out, kind, item->flags, false);
    }

    fprintf(out, ",\"fields\":{");
    for (unsigned int f = 0; f < kind->field_count; f++) {
        const struct codec_field_desc *fd = &kind->fields[f];
        const struct codec_value *v = &item->values[f];

        if (!v->set) {
            continue;
        }
        fprintf(out, "%s", first ? "" : ",");
        json_str(out, fd->name);
        fputc(':', out);
        switch (fd->type) {
        case CODEC_UINT:
            fprintf(out, "%" PRIu64, v->u);
            break;
        case CODEC_INT:
            fprintf(out, "%" PRId64, v->i);
            break;
        case CODEC_REAL:
            fprintf(out, "%.9g", v->r);
            break;
        case CODEC_TEXT:
            json_str(out, v->s ? v->s : "");
            break;
        case CODEC_BOOL:
            fprintf(out, "%s", v->b ? "true" : "false");
            break;
        default:
            fprintf(out, "null");
            break;
        }
        first = false;
    }
    fputc('}', out);

    if (item->word_count) {
        fprintf(out, ",\"words\":[");
        for (size_t i = 0; i < item->word_count; i++) {
            const struct codec_word *w = &item->words[i];

            fprintf(out, "%s{\"lane\":", i ? "," : "");
            json_str(out, w->lane < kind->lane_count ? kind->lanes[w->lane] : "?");
            fprintf(out,
                    ",\"value\":%" PRIu64 ",\"bits\":%u,\"start\":%" PRIu64 ",\"end\":%" PRIu64,
                    w->value, w->bits, w->start, w->end);
            if (w->flags & ~CODEC_WORD_BREAK) {
                fprintf(out, ",\"flags\":");
                json_flags(out, kind, w->flags & ~CODEC_WORD_BREAK, true);
            }
            fputc('}', out);
        }
        fputc(']', out);
    }

    first = true;
    for (const struct codec_item *c = item->child; c; c = c->next) {
        if (c->detail > opt->level) {
            continue;
        }
        fprintf(out, "%s", first ? ",\"children\":[" : ",");
        json_item(out, c, opt);
        first = false;
    }
    if (!first) {
        fputc(']', out);
    }
    fputc('}', out);
}


static void json_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\') {
            fprintf(out, "\\%c", c);
        } else if (c < 0x20) {
            fprintf(out, "\\u%04x", c);
        } else {
            fputc(c, out);
        }
    }
    fputc('"', out);
}


static void json_flags(FILE *out, const struct codec_kind *kind, uint32_t flags, bool word)
{
    bool first = true;

    fputc('[', out);
    for (unsigned int b = 0; b < 32; b++) {
        const char *name = NULL;

        if (!((flags >> b) & 1)) {
            continue;
        }
        name = word ? codec_word_flag_name(kind, b) : codec_flag_name(kind, b);
        fprintf(out, "%s", first ? "" : ",");
        if (name) {
            json_str(out, name);
        } else {
            fprintf(out, "\"bit%u\"", b);
        }
        first = false;
    }
    fputc(']', out);
}
