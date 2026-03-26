#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include "auth.h"
#include "document.h"
#include "protocol.h"

static int b64_encode(const unsigned char *in, int len, char *out, int outlen) {
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    BIO_write(b64, in, len);
    BIO_flush(b64);
    char *ptr; long n = BIO_get_mem_data(mem, &ptr);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, ptr, n); out[n] = '\0';
    BIO_free_all(b64);
    return (int)n;
}

static int ws_send(int fd, const char *payload, int plen) {
    unsigned char frame[MAX_CONTENT + 16];
    int i = 0;
    frame[i++] = 0x81;
    if (plen <= 125) {
        frame[i++] = (unsigned char)plen;
    } else if (plen <= 65535) {
        frame[i++] = 126;
        frame[i++] = (plen >> 8) & 0xFF;
        frame[i++] = plen & 0xFF;
    } else {
        frame[i++] = 127;
        for (int s = 56; s >= 0; s -= 8) frame[i++] = (plen >> s) & 0xFF;
    }
    memcpy(frame + i, payload, plen);
    return send(fd, frame, i + plen, MSG_NOSIGNAL) > 0 ? 0 : -1;
}

static int ws_recv(int fd, char *buf, int buflen) {
    unsigned char hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return -1;
    int opcode = hdr[0] & 0x0F;
    if (opcode == 8) return -1;
    if (opcode == 9) { unsigned char p[2]={0x8A,0}; send(fd,p,2,MSG_NOSIGNAL); return 0; }
    int masked = (hdr[1] & 0x80) != 0;
    uint64_t plen = hdr[1] & 0x7F;
    if (plen == 126) { unsigned char e[2]; recv(fd,e,2,MSG_WAITALL); plen=((uint64_t)e[0]<<8)|e[1]; }
    else if (plen == 127) { unsigned char e[8]; recv(fd,e,8,MSG_WAITALL); plen=0; for(int k=0;k<8;k++) plen=(plen<<8)|e[k]; }
    unsigned char mask[4]={0};
    if (masked) recv(fd, mask, 4, MSG_WAITALL);
    if ((int)plen >= buflen) plen = buflen - 1;
    if (recv(fd, buf, (int)plen, MSG_WAITALL) != (ssize_t)plen) return -1;
    if (masked) for (uint64_t k=0; k<plen; k++) buf[k] ^= mask[k%4];
    buf[plen] = '\0';
    return (int)plen;
}

static int json_get_str(const char *json, const char *key, char *out, int outlen) {
    char search[128]; snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p==' '||*p==':') p++;
    if (*p=='"') {
        p++; int i=0;
        while (*p && *p!='"' && i<outlen-1) {
            if (*p=='\\') { p++; if(*p) out[i++]=*p++; } else out[i++]=*p++;
        }
        out[i]='\0'; return 1;
    }
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char search[128]; snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p==' '||*p==':') p++;
    *out = atoi(p); return 1;
}

static int json_escape(const char *in, char *out, int outlen) {
    int i=0, o=0;
    while (in[i] && o<outlen-2) {
        unsigned char c=(unsigned char)in[i++];
        if      (c=='"')  { out[o++]='\\'; out[o++]='"'; }
        else if (c=='\\') { out[o++]='\\'; out[o++]='\\'; }
        else if (c=='\n') { out[o++]='\\'; out[o++]='n'; }
        else if (c=='\r') { out[o++]='\\'; out[o++]='r'; }
        else if (c=='\t') { out[o++]='\\'; out[o++]='t'; }
        else if (c<0x20)  { o+=snprintf(out+o,outlen-o,"\\u%04x",c); }
        else              { out[o++]=c; }
    }
    out[o]='\0'; return o;
}

/* ── state ── */
typedef struct {
    int       fd;
    char      username[MAX_USERNAME];
    Role      role;
    int       authenticated;
    int       active;
    int       cursor_pos;
    int       doc_idx;
    int       chars_typed;
    pthread_t thread;
} Client;

static Client          clients[MAX_USERS];
static pthread_mutex_t clients_mu = PTHREAD_MUTEX_INITIALIZER;

#define MAX_DOCS 16
static Document        docs[MAX_DOCS];
static int             doc_count = 0;
static pthread_mutex_t doc_mu    = PTHREAD_MUTEX_INITIALIZER;

static Document *get_or_create_doc(const char *name) {
    for (int i=0; i<doc_count; i++)
        if (strcmp(docs[i].name, name)==0) return &docs[i];
    if (doc_count >= MAX_DOCS) return &docs[0];
    Document *d = &docs[doc_count++];
    doc_init(d, name);
    char path[280]; snprintf(path, sizeof(path), "%s.rte", name);
    doc_load(d, path);
    return d;
}

static int doc_index(Document *d) {
    for (int i=0; i<doc_count; i++) if (&docs[i]==d) return i;
    return 0;
}

/* ── broadcast ── */
static void broadcast_to_doc(const char *json, int exclude_fd, int didx) {
    pthread_mutex_lock(&clients_mu);
    for (int i=0; i<MAX_USERS; i++)
        if (clients[i].active && clients[i].authenticated &&
            clients[i].fd!=exclude_fd && clients[i].doc_idx==didx)
            ws_send(clients[i].fd, json, (int)strlen(json));
    pthread_mutex_unlock(&clients_mu);
}

static void broadcast_all(const char *json) {
    pthread_mutex_lock(&clients_mu);
    for (int i=0; i<MAX_USERS; i++)
        if (clients[i].active && clients[i].authenticated)
            ws_send(clients[i].fd, json, (int)strlen(json));
    pthread_mutex_unlock(&clients_mu);
}

/* ── JSON builders ── */
static void send_doc_list(int fd) {
    char json[4096]; int off=0;
    off += snprintf(json+off, sizeof(json)-off, "{\"type\":\"doc_list\",\"docs\":[");
    pthread_mutex_lock(&doc_mu);
    for (int i=0; i<doc_count; i++) {
        int w,ch,l; doc_stats(&docs[i],&w,&ch,&l);
        off += snprintf(json+off, sizeof(json)-off,
            "%s{\"name\":\"%s\",\"words\":%d,\"chars\":%d}",
            i?",":"", docs[i].name, w, ch);
    }
    pthread_mutex_unlock(&doc_mu);
    off += snprintf(json+off, sizeof(json)-off, "]}");
    ws_send(fd, json, off);
}

static void send_doc_state(int fd, Document *d) {
    static char escaped[MAX_CONTENT*2];
    pthread_mutex_lock(&doc_mu);
    json_escape(d->content, escaped, sizeof(escaped));
    int w,ch,l; doc_stats(d,&w,&ch,&l);
    char json[MAX_CONTENT*2+512];
    int n = snprintf(json, sizeof(json),
        "{\"type\":\"doc_state\",\"name\":\"%s\",\"content\":\"%s\",\"seq\":%u,"
        "\"lock_holder\":\"%s\",\"words\":%d,\"chars\":%d,\"lines\":%d}",
        d->name, escaped, d->seq, d->lock_holder, w, ch, l);
    pthread_mutex_unlock(&doc_mu);
    ws_send(fd, json, n);
}

static void broadcast_stats(Document *d) {
    int didx = doc_index(d);
    int w,ch,l;
    pthread_mutex_lock(&doc_mu);
    doc_stats(d,&w,&ch,&l);
    char lock[MAX_USERNAME]; strncpy(lock, d->lock_holder, MAX_USERNAME-1);
    pthread_mutex_unlock(&doc_mu);
    pthread_mutex_lock(&clients_mu);
    int online=0;
    for (int i=0; i<MAX_USERS; i++)
        if (clients[i].active && clients[i].authenticated && clients[i].doc_idx==didx) online++;
    pthread_mutex_unlock(&clients_mu);
    char json[256];
    snprintf(json, sizeof(json),
        "{\"type\":\"stats\",\"words\":%d,\"chars\":%d,\"lines\":%d,"
        "\"online\":%d,\"lock_holder\":\"%s\"}", w,ch,l,online,lock);
    broadcast_to_doc(json, -1, didx);
}

static void broadcast_presence(Document *d) {
    int didx = doc_index(d);
    char json[MAX_USERS*160+64]; int off=0;
    off += snprintf(json+off, sizeof(json)-off, "{\"type\":\"presence\",\"users\":[");
    pthread_mutex_lock(&clients_mu);
    int first=1;
    for (int i=0; i<MAX_USERS; i++) {
        if (clients[i].active && clients[i].authenticated && clients[i].doc_idx==didx) {
            off += snprintf(json+off, sizeof(json)-off,
                "%s{\"username\":\"%s\",\"cursor\":%d,\"role\":\"%s\",\"chars\":%d}",
                first?"":","  , clients[i].username, clients[i].cursor_pos,
                role_str(clients[i].role), clients[i].chars_typed);
            first=0;
        }
    }
    pthread_mutex_unlock(&clients_mu);
    off += snprintf(json+off, sizeof(json)-off, "]}");
    broadcast_to_doc(json, -1, didx);
}

static void serve_http(int fd, const char *path) {
    const char *filename = (strncmp(path,"/",2)==0||strncmp(path,"/index.html",11)==0)
                           ? "index.html" : path+1;
    FILE *f = fopen(filename, "r");
    if (!f) { const char *r="HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found"; send(fd,r,strlen(r),MSG_NOSIGNAL); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *body=malloc(sz+1); fread(body,1,sz,f); fclose(f);
    char hdr[256];
    int hlen=snprintf(hdr,sizeof(hdr),"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\n\r\n",sz);
    send(fd,hdr,hlen,MSG_NOSIGNAL); send(fd,body,sz,MSG_NOSIGNAL); free(body);
}

static void *client_thread(void *arg) {
    Client *c = (Client *)arg;
    int cidx = (int)(c - clients);
    char buf[MAX_CONTENT+512];
    Document *cur_doc = NULL;

    char peek[8]; recv(c->fd, peek, 4, MSG_PEEK);
    if (strncmp(peek,"GET ",4)==0) {
        int n=recv(c->fd,buf,sizeof(buf)-1,0); buf[n]='\0';
        if (strstr(buf,"Upgrade: websocket")||strstr(buf,"Upgrade: WebSocket")) {
            char *ks=strstr(buf,"Sec-WebSocket-Key:"); if(!ks) goto done;
            ks+=18; while(*ks==' ') ks++;
            char ws_key[128]={0}; int ki=0;
            while(ks[ki]&&ks[ki]!='\r'&&ks[ki]!='\n'&&ki<127){ws_key[ki]=ks[ki];ki++;}
            char combined[256];
            snprintf(combined,sizeof(combined),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",ws_key);
            unsigned char sha[20]; SHA1((unsigned char*)combined,strlen(combined),sha);
            char accept[64]; b64_encode(sha,20,accept,sizeof(accept));
            char resp[512];
            int rlen=snprintf(resp,sizeof(resp),
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",accept);
            send(c->fd,resp,rlen,MSG_NOSIGNAL);
        } else {
            char path[256]="/"; sscanf(buf,"GET %255s ",path);
            serve_http(c->fd,path); goto done;
        }
    }

    while (1) {
        int n=ws_recv(c->fd,buf,sizeof(buf));
        if (n<0) break; if (n==0) continue;
        char type[64]={0}; json_get_str(buf,"type",type,sizeof(type));

        if (strcmp(type,MSG_AUTH_REQ)==0) {
            char user[MAX_USERNAME],pass[MAX_PASSWORD];
            json_get_str(buf,"username",user,sizeof(user));
            json_get_str(buf,"password",pass,sizeof(pass));
            Role role;
            if (auth_check(user,pass,&role)) {
                strncpy(c->username,user,MAX_USERNAME-1);
                c->role=role; c->authenticated=1; c->doc_idx=0;
                char resp[256];
                snprintf(resp,sizeof(resp),"{\"type\":\"auth_ok\",\"username\":\"%s\",\"role\":\"%s\"}",user,role_str(role));
                ws_send(c->fd,resp,strlen(resp));
                cur_doc=&docs[0];
                send_doc_list(c->fd);
                send_doc_state(c->fd,cur_doc);
                broadcast_presence(cur_doc);
                broadcast_stats(cur_doc);
                printf("[server] %s joined (%s)\n",user,role_str(role));
            } else {
                ws_send(c->fd,"{\"type\":\"auth_fail\",\"msg\":\"Invalid credentials\"}",48);
            }
            continue;
        }

        if (!c->authenticated) continue;

        if (strcmp(type,"switch_doc")==0) {
            char dname[MAX_DOC_NAME]={0};
            json_get_str(buf,"name",dname,sizeof(dname));
            for (char *p=dname;*p;p++) if(*p=='/'||*p=='\\'||*p=='.') *p='_';
            if (!dname[0]) continue;
            if (cur_doc) broadcast_presence(cur_doc);
            pthread_mutex_lock(&doc_mu);
            cur_doc=get_or_create_doc(dname);
            pthread_mutex_unlock(&doc_mu);
            c->doc_idx=doc_index(cur_doc);
            send_doc_state(c->fd,cur_doc);
            broadcast_presence(cur_doc);
            broadcast_stats(cur_doc);
            send_doc_list(c->fd);
            continue;
        }

        if (!cur_doc) continue;
        int didx=doc_index(cur_doc);

        if (strcmp(type,MSG_EDIT)==0) {
            if (c->role==ROLE_VIEWER) { ws_send(c->fd,"{\"type\":\"error\",\"msg\":\"Read-only\"}",33); continue; }
            char op[16],text[MAX_CONTENT]; int pos=0,len=0;
            json_get_str(buf,"op",op,sizeof(op));
            json_get_str(buf,"text",text,sizeof(text));
            json_get_int(buf,"pos",&pos);
            json_get_int(buf,"len",&len);
            pthread_mutex_lock(&doc_mu);
            if (cur_doc->seq%20==0) doc_snapshot(cur_doc);
            if (strcmp(op,OP_INSERT)==0) { doc_insert(cur_doc,pos,text); c->chars_typed+=(int)strlen(text); }
            else if (strcmp(op,OP_DELETE)==0) doc_delete(cur_doc,pos,len);
            uint32_t new_seq=cur_doc->seq;
            pthread_mutex_unlock(&doc_mu);
            char fwd[MAX_CONTENT+256], esc[MAX_CONTENT*2];
            json_escape(text,esc,sizeof(esc));
            snprintf(fwd,sizeof(fwd),
                "{\"type\":\"edit\",\"op\":\"%s\",\"pos\":%d,\"len\":%d,\"text\":\"%s\",\"username\":\"%s\",\"seq\":%u}",
                op,pos,len,esc,c->username,new_seq);
            broadcast_to_doc(fwd,c->fd,didx);
            broadcast_stats(cur_doc);
            continue;
        }

        if (strcmp(type,MSG_CURSOR)==0) {
            int pos=0; json_get_int(buf,"pos",&pos);
            pthread_mutex_lock(&clients_mu); c->cursor_pos=pos; pthread_mutex_unlock(&clients_mu);
            broadcast_presence(cur_doc); continue;
        }

        if (strcmp(type,MSG_LOCK_REQ)==0) {
            pthread_mutex_lock(&doc_mu);
            char resp[128];
            if (cur_doc->lock_holder[0]=='\0'||strcmp(cur_doc->lock_holder,c->username)==0) {
                strncpy(cur_doc->lock_holder,c->username,MAX_USERNAME-1);
                snprintf(resp,sizeof(resp),"{\"type\":\"lock_ok\",\"holder\":\"%s\"}",c->username);
                ws_send(c->fd,resp,strlen(resp));
                broadcast_to_doc(resp,c->fd,didx);
            } else {
                snprintf(resp,sizeof(resp),"{\"type\":\"lock_deny\",\"holder\":\"%s\"}",cur_doc->lock_holder);
                ws_send(c->fd,resp,strlen(resp));
            }
            pthread_mutex_unlock(&doc_mu); continue;
        }

        if (strcmp(type,MSG_UNLOCK)==0) {
            pthread_mutex_lock(&doc_mu);
            if (strcmp(cur_doc->lock_holder,c->username)==0) cur_doc->lock_holder[0]='\0';
            pthread_mutex_unlock(&doc_mu);
            broadcast_to_doc("{\"type\":\"unlock\"}",-1,didx); continue;
        }

        if (strcmp(type,MSG_SAVE)==0) {
            pthread_mutex_lock(&doc_mu); doc_save(cur_doc,cur_doc->filepath); pthread_mutex_unlock(&doc_mu);
            char resp[128]; snprintf(resp,sizeof(resp),"{\"type\":\"saved\",\"file\":\"%s\"}",cur_doc->filepath);
            ws_send(c->fd,resp,strlen(resp)); continue;
        }

        if (strcmp(type,MSG_PING)==0) { ws_send(c->fd,"{\"type\":\"pong\"}",15); continue; }
    }

done:
    pthread_mutex_lock(&doc_mu);
    if (cur_doc && strcmp(cur_doc->lock_holder,c->username)==0) cur_doc->lock_holder[0]='\0';
    pthread_mutex_unlock(&doc_mu);
    if (c->authenticated) {
        printf("[server] %s left\n",c->username);
        if (cur_doc) { broadcast_presence(cur_doc); broadcast_stats(cur_doc); }
    }
    close(c->fd);
    pthread_mutex_lock(&clients_mu); c->active=0; c->authenticated=0; pthread_mutex_unlock(&clients_mu);
    return NULL;
}

int main(int argc, char *argv[]) {
    const char *docname = argc>1 ? argv[1] : "untitled";
    int port = HTTP_PORT;
    const char *pe = getenv("PORT"); if (pe) port=atoi(pe);
    signal(SIGPIPE, SIG_IGN);
    auth_init("users.db");
    memset(docs,0,sizeof(docs)); memset(clients,0,sizeof(clients));
    doc_init(&docs[0],docname);
    char docpath[280]; snprintf(docpath,sizeof(docpath),"%s.rte",docname);
    if (!doc_load(&docs[0],docpath)) printf("[server] New document: %s\n",docname);
    else printf("[server] Loaded: %s (%d chars)\n",docname,docs[0].length);
    doc_count=1;
    int srv=socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=INADDR_ANY,.sin_port=htons(port)};
    if (bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){perror("bind");return 1;}
    listen(srv,MAX_USERS);
    printf("[server] Listening on http://localhost:%d\n",port);
    printf("[server] Accounts: admin/admin123  alice/alice123  bob/bob123  carol/carol123(viewer)\n");
    while (1) {
        struct sockaddr_in ca; socklen_t cl=sizeof(ca);
        int cfd=accept(srv,(struct sockaddr*)&ca,&cl); if(cfd<0) continue;
        pthread_mutex_lock(&clients_mu);
        Client *slot=NULL;
        for (int i=0;i<MAX_USERS;i++) if(!clients[i].active){slot=&clients[i];break;}
        if (!slot){pthread_mutex_unlock(&clients_mu);close(cfd);continue;}
        memset(slot,0,sizeof(*slot)); slot->fd=cfd; slot->active=1;
        pthread_mutex_unlock(&clients_mu);
        pthread_create(&slot->thread,NULL,client_thread,slot);
        pthread_detach(slot->thread);
    }
    auth_save("users.db"); close(srv); return 0;
}
