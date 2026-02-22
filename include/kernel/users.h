/*
 * include/kernel/users.h — User account system
 *
 * Simple 16-user flat database stored at /sys/users/db.
 * Passwords are hashed with a djb2-based hash (for educational OS use).
 * uid 0 is the system/root-equivalent (capabilities still required).
 */
#pragma once
#include <types.h>

#define USERS_MAX          16
#define USER_NAME_LEN      32
#define USER_HOME_LEN      64
#define USER_SHELL_LEN     32
#define USER_DB_PATH       "/sys/users/db"
#define USER_HASH_LEN      16   /* hex string: 8 bytes → 16 chars */

typedef struct {
    uint32_t uid;
    uint32_t gid;
    char     name[USER_NAME_LEN];
    char     pw_hash[USER_HASH_LEN + 1];  /* hex djb2 hash */
    char     home[USER_HOME_LEN];
    char     shell[USER_SHELL_LEN];
    bool     active;
    bool     locked;
} user_t;

/* Initialize user system. Creates default 'root' and 'user' accounts
 * if the DB doesn't exist. */
void users_init(void);

/* Authenticate: returns uid on success, -1 on failure */
int  users_authenticate(const char* name, const char* password);

/* Get user by uid / name */
const user_t* users_get_by_uid(uint32_t uid);
const user_t* users_get_by_name(const char* name);

/* Create a new user. Returns uid or -1 on error. */
int  users_create(const char* name, const char* password,
                   const char* home, const char* shell);

/* Change password. Must provide old password (or be uid 0). */
int  users_chpasswd(uint32_t uid, const char* old_pw,
                    const char* new_pw);

/* Lock / unlock account */
void users_lock(uint32_t uid);
void users_unlock(uint32_t uid);

/* Save DB to VFS */
void users_save(void);

/* Return all users (for display) */
int  users_count(void);
const user_t* users_list(void);
