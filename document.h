#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <stdint.h>

#define MAX_DOC_NAME 64
#define MAX_CONTENT  65536
#define MAX_HISTORY  64

typedef struct {
    char    content[MAX_CONTENT];
    int     length;
    uint32_t seq;
} DocSnapshot;

typedef struct {
    char        name[MAX_DOC_NAME];
    char        content[MAX_CONTENT];
    int         length;
    uint32_t    seq;
    char        lock_holder[32];
    DocSnapshot history[MAX_HISTORY];
    int         history_count;
    char        filepath[256];
} Document;

void doc_init(Document *d, const char *name);
int  doc_load(Document *d, const char *path);
int  doc_save(Document *d, const char *path);
void doc_insert(Document *d, int pos, const char *text);
void doc_delete(Document *d, int pos, int len);
void doc_snapshot(Document *d);
void doc_stats(const Document *d, int *words, int *chars, int *lines);

#endif
