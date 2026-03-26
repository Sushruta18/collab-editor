#include <stdio.h>
#include <string.h>
#include "auth.h"

#define MAX_STORED_USERS 64

static UserRecord users[MAX_STORED_USERS];
static int user_count = 0;

static void simple_hash(const char *pw, char out[65]) {
    unsigned long h = 5381;
    for (const char *p = pw; *p; p++)
        h = ((h << 5) + h) ^ (unsigned char)*p;
    snprintf(out, 65, "%016lx%016lx%016lx%016lx",
             h, h ^ 0xDEADBEEFUL, h ^ 0xCAFEBABEUL, h ^ 0x12345678UL);
}

static void add(const char *u, const char *pw, Role r) {
    if (user_count >= MAX_STORED_USERS) return;
    UserRecord *rec = &users[user_count++];
    strncpy(rec->username, u, MAX_USERNAME - 1);
    simple_hash(pw, rec->password_hash);
    rec->role = r; rec->active = 1;
}

int auth_init(const char *userfile) {
    FILE *f = fopen(userfile, "r");
    if (!f) {
        add("admin", "admin123", ROLE_ADMIN);
        add("alice", "alice123", ROLE_EDITOR);
        add("bob",   "bob123",   ROLE_EDITOR);
        add("carol", "carol123", ROLE_VIEWER);
        return 0;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && user_count < MAX_STORED_USERS) {
        UserRecord *u = &users[user_count];
        int role;
        if (sscanf(line, "%31s %64s %d", u->username, u->password_hash, &role) == 3) {
            u->role = (Role)role; u->active = 1; user_count++;
        }
    }
    fclose(f);
    return user_count;
}

int auth_check(const char *username, const char *password, Role *out_role) {
    char hash[65]; simple_hash(password, hash);
    for (int i = 0; i < user_count; i++) {
        if (users[i].active &&
            strcmp(users[i].username, username) == 0 &&
            strcmp(users[i].password_hash, hash) == 0) {
            if (out_role) *out_role = users[i].role;
            return 1;
        }
    }
    return 0;
}

void auth_save(const char *userfile) {
    FILE *f = fopen(userfile, "w");
    if (!f) return;
    for (int i = 0; i < user_count; i++)
        if (users[i].active)
            fprintf(f, "%s %s %d\n", users[i].username, users[i].password_hash, users[i].role);
    fclose(f);
}

const char *role_str(Role r) {
    switch (r) {
        case ROLE_VIEWER: return "viewer";
        case ROLE_EDITOR: return "editor";
        case ROLE_ADMIN:  return "admin";
        default:          return "unknown";
    }
}
