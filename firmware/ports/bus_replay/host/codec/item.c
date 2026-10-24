#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codec/item.h"


static const char *const common_flags[] = {"incomplete", "loss"};
static const char *const common_word_flags[] = {"break", "error"};


// A new item without a parent, starting at tick start. Its id is given when it is emitted.
struct codec_item *codec_item_new(const struct codec_dec *dec, const struct codec_kind *kind,
                                  uint64_t start)
{
    struct codec_item *item = calloc(1, sizeof(*item));

    if (!item) {
        return NULL;
    }
    item->values = calloc(kind->field_count ? kind->field_count : 1, sizeof(*item->values));
    if (!item->values) {
        free(item);
        return NULL;
    }
    item->kind = kind;
    item->dec = dec;
    item->start = start;
    item->end = start;

    return item;
}


// A new item as the last child of parent.
struct codec_item *codec_item_child(struct codec_item *parent, const struct codec_dec *dec,
                                    const struct codec_kind *kind, uint64_t start)
{
    struct codec_item *item = codec_item_new(dec, kind, start);

    if (!item) {
        return NULL;
    }
    item->parent = parent;
    if (parent->last_child) {
        parent->last_child->next = item;
    } else {
        parent->child = item;
    }
    parent->last_child = item;

    return item;
}


void codec_item_free(struct codec_item *item)
{
    struct codec_item *c = NULL;

    if (!item) {
        return;
    }
    c = item->child;
    while (c) {
        struct codec_item *next = c->next;

        codec_item_free(c);
        c = next;
    }
    for (unsigned int f = 0; f < item->kind->field_count; f++) {
        if (item->kind->fields[f].type == CODEC_TEXT) {
            free(item->values[f].s);
        }
    }
    if (item->payload && item->payload_free) {
        item->payload_free(item->payload);
    }
    free(item->values);
    free(item->words);
    free(item);
}


void codec_set_uint(struct codec_item *item, unsigned int field, uint64_t v)
{
    item->values[field].u = v;
    item->values[field].set = true;
}


void codec_set_int(struct codec_item *item, unsigned int field, int64_t v)
{
    item->values[field].i = v;
    item->values[field].set = true;
}


void codec_set_real(struct codec_item *item, unsigned int field, double v)
{
    item->values[field].r = v;
    item->values[field].set = true;
}


void codec_set_bool(struct codec_item *item, unsigned int field, bool v)
{
    item->values[field].b = v;
    item->values[field].set = true;
}


int codec_set_text(struct codec_item *item, unsigned int field, const char *s)
{
    char *copy = strdup(s);

    if (!copy) {
        return -ENOMEM;
    }
    free(item->values[field].s);
    item->values[field].s = copy;
    item->values[field].set = true;

    return 0;
}


int codec_word_add(struct codec_item *item, const struct codec_word *w)
{
    if (item->word_count == item->word_cap) {
        size_t cap = item->word_cap ? 2 * item->word_cap : 16;
        struct codec_word *bigger = realloc(item->words, cap * sizeof(*bigger));

        if (!bigger) {
            return -ENOMEM;
        }
        item->words = bigger;
        item->word_cap = cap;
    }
    item->words[item->word_count++] = *w;

    return 0;
}


// The name of item flag bit (0 to 31), or NULL for an unnamed one.
const char *codec_flag_name(const struct codec_kind *kind, unsigned int bit)
{
    if (bit < sizeof(common_flags) / sizeof(common_flags[0])) {
        return common_flags[bit];
    }
    if (bit < CODEC_ITEM_KIND_SHIFT || !kind->item_flags) {
        return NULL;
    }
    for (unsigned int i = 0; kind->item_flags[i]; i++) {
        if (i == bit - CODEC_ITEM_KIND_SHIFT) {
            return kind->item_flags[i];
        }
    }

    return NULL;
}


// The name of word flag bit (0 to 7), or NULL for an unnamed one.
const char *codec_word_flag_name(const struct codec_kind *kind, unsigned int bit)
{
    if (bit < sizeof(common_word_flags) / sizeof(common_word_flags[0])) {
        return common_word_flags[bit];
    }
    if (bit < CODEC_WORD_KIND_SHIFT || !kind->word_flags) {
        return NULL;
    }
    for (unsigned int i = 0; kind->word_flags[i]; i++) {
        if (i == bit - CODEC_WORD_KIND_SHIFT) {
            return kind->word_flags[i];
        }
    }

    return NULL;
}
