#ifndef AUTH_H
#define AUTH_H

#define MAX_USERNAME 32
#define MAX_PASSWORD 64

typedef enum { ROLE_VIEWER = 1, ROLE_EDITOR, ROLE_ADMIN } Role;

typedef struct {
    char username[MAX_USERNAME];
    char password_hash[65];
    Role role;
    int  active;
} UserRecord;

int         auth_init(const char *userfile);
int         auth_check(const char *username, const char *password, Role *out_role);
void        auth_save(const char *userfile);
const char *role_str(Role r);

#endif
