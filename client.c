#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <signal.h>
#include <errno.h>
#include "protocol.h"

/* ── local document mirror ──────────────────────────────────────── */
static char    doc_content[MAX_CONTENT];
static uint8_t doc_fmt[MAX_CONTENT];
static int     doc_length   = 0;
static uint32_t doc_seq     = 0;
static char    doc_name[MAX_DOC_NAME];
static char    lock_holder[MAX_USERNAME];

/* ── cursor & selection ─────────────────────────────────────────── */
static int cursor_pos  = 0;
static int sel_start   = -1;
static int sel_end     = -1;
static int has_lock    = 0;

/* ── remote cursors ─────────────────────────────────────────────── */
static UserPresence remote_cursors[MAX_USERS];
static int          remote_count = 0;
static pthread_mutex_t state_mu = PTHREAD_MUTEX_INITIALIZER;

/* ── stats ──────────────────────────────────────────────────────── */
static int stat_words = 0, stat_chars = 0, stat_lines = 0, stat_users = 0;
static char stat_lock[MAX_USERNAME];

/* ── current format for new input ──────────────────────────────── */
static uint8_t cur_fmt = FMT_NONE;

/* ── network ────────────────────────────────────────────────────── */
static int    sockfd = -1;
static char   my_username[MAX_USERNAME];
static int    running = 1;

/* ── ncurses windows ────────────────────────────────────────────── */
static WINDOW *win_doc    = NULL;
static WINDOW *win_status = NULL;
static WINDOW *win_help   = NULL;

/* ── helpers ────────────────────────────────────────────────────── */
static int send_msg(const Message *m) {
    return send(sockfd, m, sizeof(Message), MSG_NOSIGNAL) == sizeof(Message) ? 0 : -1;
}
static int recv_msg(Message *m) {
    return recv(sockfd, m, sizeof(Message), MSG_WAITALL) == sizeof(Message) ? 0 : -1;
}

static void apply_edit(const EditOp *op) {
    int pos = op->pos;
    if (pos < 0) pos = 0;
    if (pos > doc_length) pos = doc_length;
    if (op->op == OP_INSERT) {
        int n = (int)strlen(op->text);
        if (doc_length + n >= MAX_CONTENT) return;
        memmove(doc_content + pos + n, doc_content + pos, doc_length - pos);
        memmove(doc_fmt     + pos + n, doc_fmt     + pos, doc_length - pos);
        memcpy(doc_content + pos, op->text, n);
        memset(doc_fmt     + pos, op->fmt,  n);
        doc_length += n; doc_content[doc_length] = '\0';
        if (cursor_pos >= pos) cursor_pos += n;
    } else if (op->op == OP_DELETE) {
        int n = op->len;
        if (pos + n > doc_length) n = doc_length - pos;
        memmove(doc_content + pos, doc_content + pos + n, doc_length - pos - n);
        memmove(doc_fmt     + pos, doc_fmt     + pos + n, doc_length - pos - n);
        doc_length -= n; doc_content[doc_length] = '\0';
        if (cursor_pos > pos) cursor_pos -= (cursor_pos < pos + n) ? cursor_pos - pos : n;
    } else if (op->op == OP_FORMAT) {
        int end = pos + op->len; if (end > doc_length) end = doc_length;
        for (int i = pos; i < end; i++) doc_fmt[i] = op->fmt;
    }
}

/* ── receiver thread ────────────────────────────────────────────── */
static void *recv_thread(void *arg) {
    (void)arg;
    Message m;
    while (running && recv_msg(&m) == 0) {
        pthread_mutex_lock(&state_mu);
        switch ((MsgType)m.type) {
        case MSG_DOC_STATE:
            doc_length = m.payload.doc_state.length;
            memcpy(doc_content, m.payload.doc_state.content, doc_length);
            memcpy(doc_fmt,     m.payload.doc_state.fmt,     doc_length);
            doc_content[doc_length] = '\0';
            strncpy(doc_name,    m.payload.doc_state.docname,     MAX_DOC_NAME - 1);
            strncpy(lock_holder, m.payload.doc_state.lock_holder, MAX_USERNAME - 1);
            cursor_pos = 0;
            break;
        case MSG_EDIT:
            apply_edit(&m.payload.edit.op);
            doc_seq = m.payload.edit.doc_seq;
            break;
        case MSG_CURSOR: {
            UserPresence *p = &m.payload.presence;
            int found = 0;
            for (int i = 0; i < remote_count; i++) {
                if (strcmp(remote_cursors[i].username, p->username) == 0) {
                    remote_cursors[i] = *p; found = 1; break;
                }
            }
            if (!found && remote_count < MAX_USERS) remote_cursors[remote_count++] = *p;
            break;
        }
        case MSG_PRESENCE:
            if (!m.payload.presence_list.joined) {
                /* Remove from remote cursors */
                for (int i = 0; i < remote_count; i++) {
                    if (strcmp(remote_cursors[i].username, m.payload.presence_list.username) == 0) {
                        remote_cursors[i] = remote_cursors[--remote_count]; break;
                    }
                }
            }
            break;
        case MSG_DOC_LOCK_OK:
            has_lock = 1; lock_holder[0] = '\0';
            strncpy(lock_holder, my_username, MAX_USERNAME - 1);
            break;
        case MSG_DOC_LOCK_DENY:
            has_lock = 0;
            strncpy(lock_holder, m.payload.lock_info.holder, MAX_USERNAME - 1);
            break;
        case MSG_DOC_UNLOCK:
            if (strcmp(lock_holder, my_username) != 0) lock_holder[0] = '\0';
            break;
        case MSG_STATS:
            stat_words = m.payload.stats.words;
            stat_chars = m.payload.stats.chars;
            stat_lines = m.payload.stats.lines;
            stat_users = m.payload.stats.online_users;
            strncpy(stat_lock, m.payload.stats.lock_holder, MAX_USERNAME - 1);
            break;
        case MSG_AUTH_OK:
            /* Save/info messages — ignore silently */
            break;
        default: break;
        }
        pthread_mutex_unlock(&state_mu);
    }
    running = 0;
    return NULL;
}

/* ── ncurses rendering ──────────────────────────────────────────── */
static int attr_for_fmt(uint8_t fmt) {
    int a = A_NORMAL;
    if (fmt & FMT_BOLD)      a |= A_BOLD;
    if (fmt & FMT_ITALIC)    a |= A_DIM;   /* terminal approximation */
    if (fmt & FMT_UNDERLINE) a |= A_UNDERLINE;
    if (fmt & FMT_CODE)      a |= A_REVERSE;
    return a;
}

/* Colour pairs: 1=normal, 2=remote cursor 1, 3=remote cursor 2, 4=status */
static void init_colors(void) {
    if (!has_colors()) return;
    start_color(); use_default_colors();
    init_pair(1, COLOR_WHITE,  -1);
    init_pair(2, COLOR_BLACK,  COLOR_CYAN);
    init_pair(3, COLOR_BLACK,  COLOR_YELLOW);
    init_pair(4, COLOR_BLACK,  COLOR_WHITE);
    init_pair(5, COLOR_BLACK,  COLOR_GREEN);
    init_pair(6, COLOR_WHITE,  COLOR_RED);
}

static void draw_status(void) {
    int cols = getmaxx(win_status);
    werase(win_status);
    wbkgd(win_status, COLOR_PAIR(4));
    wattron(win_status, COLOR_PAIR(4) | A_BOLD);

    char left[256], right[256];
    snprintf(left, sizeof(left), " %s | %s | Ln:%d Ch:%d Wd:%d | Users:%d",
             doc_name, has_lock ? "LOCKED(you)" : (lock_holder[0] ? lock_holder : "unlocked"),
             stat_lines, stat_chars, stat_words, stat_users);
    snprintf(right, sizeof(right),
             "B:^B I:^I U:^U Code:^K Lock:^L Save:^S Quit:^Q ");

    mvwprintw(win_status, 0, 0, "%-*s", cols, left);
    int rlen = (int)strlen(right);
    if (rlen < cols) mvwprintw(win_status, 0, cols - rlen, "%s", right);
    wattroff(win_status, COLOR_PAIR(4) | A_BOLD);
    wrefresh(win_status);
}

static void draw_doc(void) {
    int rows, cols;
    getmaxyx(win_doc, rows, cols);
    werase(win_doc);

    /* Build remote cursor position set */
    int rcur_pos[MAX_USERS]; int rcur_color[MAX_USERS]; int rcnt = 0;
    pthread_mutex_lock(&state_mu);
    for (int i = 0; i < remote_count && rcnt < MAX_USERS; i++) {
        rcur_pos[rcnt]   = remote_cursors[i].pos;
        rcur_color[rcnt] = (i % 2) + 2; /* pair 2 or 3 */
        rcnt++;
    }
    pthread_mutex_unlock(&state_mu);

    int row = 0, col = 0;
    int cur_row = 0, cur_col = 0;

    for (int i = 0; i <= doc_length && row < rows; i++) {
        if (i == cursor_pos) { cur_row = row; cur_col = col; }

        /* Check remote cursors */
        int rcolor = 0;
        for (int r = 0; r < rcnt; r++) if (rcur_pos[r] == i) { rcolor = rcur_color[r]; break; }

        if (i == doc_length) {
            if (rcolor) { wattron(win_doc, COLOR_PAIR(rcolor)); waddch(win_doc, ' '); wattroff(win_doc, COLOR_PAIR(rcolor)); }
            break;
        }

        char ch = doc_content[i];
        int  at = attr_for_fmt(doc_fmt[i]);

        /* Selection highlight */
        if (sel_start >= 0 && i >= sel_start && i < sel_end)
            at |= A_STANDOUT;

        if (rcolor) wattron(win_doc, COLOR_PAIR(rcolor));
        else        wattron(win_doc, at);

        if (ch == '\n') {
            waddch(win_doc, ' ');
            if (rcolor) wattroff(win_doc, COLOR_PAIR(rcolor));
            else        wattroff(win_doc, at);
            col = 0; row++;
            continue;
        }
        waddch(win_doc, (unsigned char)ch);
        if (rcolor) wattroff(win_doc, COLOR_PAIR(rcolor));
        else        wattroff(win_doc, at);
        col++;
        if (col >= cols) { col = 0; row++; }
    }

    wmove(win_doc, cur_row, cur_col);
    wrefresh(win_doc);
}

static void send_cursor(void) {
    Message m = {0}; m.type = MSG_CURSOR;
    strncpy(m.payload.presence.username, my_username, MAX_USERNAME - 1);
    m.payload.presence.pos       = cursor_pos;
    m.payload.presence.sel_start = sel_start;
    m.payload.presence.sel_end   = sel_end;
    m.payload.presence.active    = 1;
    send_msg(&m);
}

static void send_edit(OpType op, int pos, const char *text, int len, uint8_t fmt) {
    Message m = {0}; m.type = MSG_EDIT;
    strncpy(m.payload.edit.username, my_username, MAX_USERNAME - 1);
    m.payload.edit.op.op  = op;
    m.payload.edit.op.pos = pos;
    m.payload.edit.op.len = len;
    m.payload.edit.op.fmt = fmt;
    if (text) strncpy(m.payload.edit.op.text, text, 511);
    m.payload.edit.doc_seq = doc_seq;
    send_msg(&m);
    /* Apply locally */
    pthread_mutex_lock(&state_mu);
    apply_edit(&m.payload.edit.op);
    doc_seq++;
    pthread_mutex_unlock(&state_mu);
}

/* ── login screen ───────────────────────────────────────────────── */
static int do_login(const char *host) {
    char password[MAX_PASSWORD];
    clear();
    mvprintw(2, 2, "=== Collaborative Rich Text Editor ===");
    mvprintw(4, 2, "Server: %s:%d", host, PORT);
    mvprintw(6, 2, "Username: "); refresh();
    echo(); curs_set(1);
    getnstr(my_username, MAX_USERNAME - 1);
    mvprintw(7, 2, "Password: "); refresh();
    noecho();
    getnstr(password, MAX_PASSWORD - 1);
    noecho(); curs_set(0);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        mvprintw(9, 2, "Connection failed: %s", strerror(errno));
        refresh(); sleep(2); return 0;
    }
    Message m = {0}, reply;
    m.type = MSG_AUTH_REQ;
    strncpy(m.payload.auth.username, my_username, MAX_USERNAME - 1);
    strncpy(m.payload.auth.password, password,    MAX_PASSWORD - 1);
    send_msg(&m);
    if (recv_msg(&reply) < 0 || reply.type != MSG_AUTH_OK) {
        mvprintw(9, 2, "Auth failed: %s", reply.payload.text);
        refresh(); sleep(2); close(sockfd); sockfd = -1; return 0;
    }
    mvprintw(9, 2, "%s", reply.payload.text);
    refresh(); sleep(1);
    return 1;
}

/* ── main editor loop ───────────────────────────────────────────── */
static void editor_loop(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    win_doc    = newwin(rows - 2, cols, 0, 0);
    win_status = newwin(1, cols, rows - 2, 0);
    win_help   = newwin(1, cols, rows - 1, 0);

    scrollok(win_doc, TRUE);
    keypad(win_doc, TRUE);
    nodelay(win_doc, FALSE);
    wtimeout(win_doc, 100);

    /* Show help bar */
    wbkgd(win_help, COLOR_PAIR(5));
    mvwprintw(win_help, 0, 0, " ^B Bold  ^I Italic  ^U Underline  ^K Code  ^L Lock/Unlock  ^S Save  ^Q Quit");
    wrefresh(win_help);

    while (running) {
        pthread_mutex_lock(&state_mu);
        draw_doc();
        draw_status();
        pthread_mutex_unlock(&state_mu);

        int ch = wgetch(win_doc);
        if (ch == ERR) continue;

        pthread_mutex_lock(&state_mu);

        switch (ch) {
        case 17: /* ^Q */ running = 0; break;

        case 19: /* ^S */ {
            Message m = {0}; m.type = MSG_SAVE; send_msg(&m);
            break;
        }
        case 12: /* ^L — toggle lock */ {
            Message m = {0};
            if (has_lock) { m.type = MSG_DOC_UNLOCK; has_lock = 0; lock_holder[0] = '\0'; }
            else          { m.type = MSG_DOC_LOCK_REQ; }
            send_msg(&m);
            break;
        }
        case 2:  cur_fmt ^= FMT_BOLD;      break; /* ^B */
        case 9:  cur_fmt ^= FMT_ITALIC;    break; /* ^I */
        case 21: cur_fmt ^= FMT_UNDERLINE; break; /* ^U */
        case 11: cur_fmt ^= FMT_CODE;      break; /* ^K */

        case KEY_LEFT:
            if (cursor_pos > 0) cursor_pos--;
            send_cursor(); break;
        case KEY_RIGHT:
            if (cursor_pos < doc_length) cursor_pos++;
            send_cursor(); break;
        case KEY_UP: {
            int p = cursor_pos - 1;
            while (p > 0 && doc_content[p] != '\n') p--;
            cursor_pos = (p > 0) ? p : 0;
            send_cursor(); break;
        }
        case KEY_DOWN: {
            int p = cursor_pos;
            while (p < doc_length && doc_content[p] != '\n') p++;
            if (p < doc_length) cursor_pos = p + 1;
            send_cursor(); break;
        }
        case KEY_HOME: {
            int p = cursor_pos - 1;
            while (p > 0 && doc_content[p] != '\n') p--;
            cursor_pos = (doc_content[p] == '\n') ? p + 1 : 0;
            send_cursor(); break;
        }
        case KEY_END: {
            while (cursor_pos < doc_length && doc_content[cursor_pos] != '\n') cursor_pos++;
            send_cursor(); break;
        }

        case KEY_BACKSPACE: case 127: case '\b':
            if (cursor_pos > 0) {
                pthread_mutex_unlock(&state_mu);
                send_edit(OP_DELETE, cursor_pos - 1, NULL, 1, 0);
                cursor_pos--;
                send_cursor();
                pthread_mutex_lock(&state_mu);
            }
            break;

        case KEY_DC: /* Delete key */
            if (cursor_pos < doc_length) {
                pthread_mutex_unlock(&state_mu);
                send_edit(OP_DELETE, cursor_pos, NULL, 1, 0);
                send_cursor();
                pthread_mutex_lock(&state_mu);
            }
            break;

        default:
            if (ch >= 32 || ch == '\n' || ch == '\t') {
                char buf[2] = { (char)ch, '\0' };
                pthread_mutex_unlock(&state_mu);
                send_edit(OP_INSERT, cursor_pos, buf, 1, cur_fmt);
                cursor_pos++;
                send_cursor();
                pthread_mutex_lock(&state_mu);
            }
            break;
        }
        pthread_mutex_unlock(&state_mu);
    }
}

/* ── entry point ────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";

    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
    init_colors(); curs_set(1);

    if (!do_login(host)) { endwin(); return 1; }

    /* Wait for initial doc state */
    Message m;
    if (recv_msg(&m) == 0 && m.type == MSG_DOC_STATE) {
        doc_length = m.payload.doc_state.length;
        memcpy(doc_content, m.payload.doc_state.content, doc_length);
        memcpy(doc_fmt,     m.payload.doc_state.fmt,     doc_length);
        doc_content[doc_length] = '\0';
        strncpy(doc_name,    m.payload.doc_state.docname,     MAX_DOC_NAME - 1);
        strncpy(lock_holder, m.payload.doc_state.lock_holder, MAX_USERNAME - 1);
    }

    pthread_t rt;
    pthread_create(&rt, NULL, recv_thread, NULL);

    clear(); refresh();
    editor_loop();

    running = 0;
    shutdown(sockfd, SHUT_RDWR);
    pthread_join(rt, NULL);
    close(sockfd);
    endwin();
    return 0;
}
