#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "document.h"

void doc_init(Document *d, const char *name) {
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, MAX_DOC_NAME - 1);
    snprintf(d->filepath, sizeof(d->filepath), "%s.rte", name);
}

int doc_load(Document *d, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fread(&d->length, sizeof(int), 1, f);
    if (d->length >= MAX_CONTENT) d->length = MAX_CONTENT - 1;
    fread(d->content, 1, d->length, f);
    d->content[d->length] = '\0';
    fclose(f);
    strncpy(d->filepath, path, sizeof(d->filepath) - 1);
    return 1;
}

int doc_save(Document *d, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(&d->length, sizeof(int), 1, f);
    fwrite(d->content, 1, d->length, f);
    fclose(f);
    return 1;
}

void doc_insert(Document *d, int pos, const char *text) {
    int n = (int)strlen(text);
    if (pos < 0) pos = 0;
    if (pos > d->length) pos = d->length;
    if (d->length + n >= MAX_CONTENT) return;
    memmove(d->content + pos + n, d->content + pos, d->length - pos);
    memcpy(d->content + pos, text, n);
    d->length += n;
    d->content[d->length] = '\0';
    d->seq++;
}

void doc_delete(Document *d, int pos, int len) {
    if (pos < 0) pos = 0;
    if (pos >= d->length) return;
    if (pos + len > d->length) len = d->length - pos;
    memmove(d->content + pos, d->content + pos + len, d->length - pos - len);
    d->length -= len;
    d->content[d->length] = '\0';
    d->seq++;
}

void doc_snapshot(Document *d) {
    if (d->history_count >= MAX_HISTORY) {
        memmove(&d->history[0], &d->history[1], sizeof(DocSnapshot) * (MAX_HISTORY - 1));
        d->history_count = MAX_HISTORY - 1;
    }
    DocSnapshot *s = &d->history[d->history_count++];
    memcpy(s->content, d->content, d->length + 1);
    s->length = d->length;
    s->seq    = d->seq;
}

void doc_stats(const Document *d, int *words, int *chars, int *lines) {
    *chars = d->length; *lines = 1; *words = 0;
    int in_word = 0;
    for (int i = 0; i < d->length; i++) {
        char c = d->content[i];
        if (c == '\n') (*lines)++;
        if (c == ' ' || c == '\n' || c == '\t') { if (in_word) { (*words)++; in_word = 0; } }
        else in_word = 1;
    }
    if (in_word) (*words)++;
}
