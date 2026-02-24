/*
 * kernel/users.c — User account management
 *
 * djb2 hash (32-bit): hash = hash * 33 ^ c, starting at 5381.
 * Hash is stored as 8 hex digits + 8 salt-derived digits (16 chars total).
 *
 * DB format (plain text for simplicity, like /etc/passwd):
 *   uid:gid:name:pw_hash:home:shell:active:locked\n
 */
#include <kernel/users.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <types.h>

static user_t   g_users[USERS_MAX];
static int      g_count = 0;

/* =========================================================
 * djb2 password hash
 * ========================================================= */
static uint32_t djb2(const char* s)
{
    uint32_t h = 5381;
    int c;
    while ((c = (unsigned char)*s++) != 0)
        h = ((h << 5) + h) ^ (uint32_t)c;
    return h;
}

static void hash_password(const char* pw, char* out)
{
    /* Simple: djb2(password) + djb2(reverse(password)) → 16 hex digits */
    uint32_t h1 = djb2(pw);

    /* Reverse pass */
    char rev[64];
    int len = (int)strlen(pw);
    if (len > 63) len = 63;
    for (int i = 0; i < len; i++) rev[i] = pw[len - 1 - i];
    rev[len] = '\0';
    uint32_t h2 = djb2(rev);

    /* Format as 16 hex chars */
    const char* hex = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[i]     = hex[h1 & 0xF]; h1 >>= 4;
        out[i + 8] = hex[h2 & 0xF]; h2 >>= 4;
    }
    out[16] = '\0';
}

static bool check_password(const user_t* u, const char* pw)
{
    char hash[USER_HASH_LEN + 1];
    hash_password(pw, hash);
    return strcmp(hash, u->pw_hash) == 0;
}

/* =========================================================
 * Number formatters (no printf)
 * ========================================================= */
static void uint_to_str(uint32_t n, char* buf)
{
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (n > 0) { tmp[i++] = '0' + (int)(n % 10); n /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static uint32_t str_to_uint(const char* s)
{
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (uint32_t)(*s++ - '0');
    return n;
}

/* =========================================================
 * DB serialise / deserialise
 * ========================================================= */
#define DB_LINE_MAX 256

static char* next_field(char* p, char* out, int max)
{
    int i = 0;
    while (*p && *p != ':' && *p != '\n' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
    if (*p == ':') p++;
    return p;
}

static void load_db(void)
{
    vfs_node_t* node = vfs_resolve_path(USER_DB_PATH);
    if (!node || !node->ops || !node->ops->read) return;

    char buf[4096];
    int n = node->ops->read(node, 0, sizeof(buf) - 1, buf);
    if (n <= 0) return;
    buf[n] = '\0';

    char* line = buf;
    while (*line && g_count < USERS_MAX) {
        char* next = line;
        while (*next && *next != '\n') next++;
        if (*next == '\n') *next++ = '\0';
        if (!*line) { line = next; continue; }

        user_t* u = &g_users[g_count];
        memset(u, 0, sizeof(*u));
        char tmp[64];

        char* p = line;
        p = next_field(p, tmp, sizeof(tmp)); u->uid = str_to_uint(tmp);
        p = next_field(p, tmp, sizeof(tmp)); u->gid = str_to_uint(tmp);
        p = next_field(p, u->name,    sizeof(u->name));
        p = next_field(p, u->pw_hash, sizeof(u->pw_hash));
        p = next_field(p, u->home,    sizeof(u->home));
        p = next_field(p, u->shell,   sizeof(u->shell));
        p = next_field(p, tmp, sizeof(tmp)); u->active = (tmp[0] == '1');
        p = next_field(p, tmp, sizeof(tmp)); u->locked = (tmp[0] == '1');

        if (u->name[0]) g_count++;
        line = next;
    }
}

void users_save(void)
{
    /* Build DB text */
    char buf[4096];
    int pos = 0;

    for (int i = 0; i < g_count; i++) {
        user_t* u = &g_users[i];
        char uid_s[12], gid_s[12];
        uint_to_str(u->uid, uid_s);
        uint_to_str(u->gid, gid_s);

        /* uid:gid:name:pw_hash:home:shell:active:locked\n */
        const char* fields[] = {
            uid_s, ":", gid_s, ":", u->name, ":", u->pw_hash, ":",
            u->home, ":", u->shell, ":",
            u->active ? "1" : "0", ":",
            u->locked ? "1" : "0", "\n",
            NULL
        };
        for (int fi = 0; fields[fi]; fi++) {
            int len = (int)strlen(fields[fi]);
            if (pos + len >= (int)sizeof(buf) - 1) break;
            memcpy(buf + pos, fields[fi], (size_t)len);
            pos += len;
        }
    }
    buf[pos] = '\0';

    /* Write to VFS */
    vfs_mkdir("/sys");
    vfs_mkdir("/sys/users");
    vfs_node_t* node = vfs_resolve_path(USER_DB_PATH);
    if (!node) {
        vfs_create(USER_DB_PATH, 0);
        node = vfs_resolve_path(USER_DB_PATH);
    }
    if (node && node->ops && node->ops->write) {
        node->size = 0;  /* truncate before overwrite */
        node->ops->write(node, 0, pos, buf);
    }
}

/* =========================================================
 * Default account creation
 * ========================================================= */
static void create_defaults(void)
{
    g_count = 0;

    /* uid 0: system */
    user_t* sys = &g_users[g_count++];
    memset(sys, 0, sizeof(*sys));
    sys->uid = 0; sys->gid = 0;
    strncpy(sys->name, "system", USER_NAME_LEN - 1);
    hash_password("aetheros", sys->pw_hash);
    strncpy(sys->home,  "/sys", USER_HOME_LEN - 1);
    strncpy(sys->shell, "/bin/ash", USER_SHELL_LEN - 1);
    sys->active = true;

    /* uid 1: default user */
    user_t* usr = &g_users[g_count++];
    memset(usr, 0, sizeof(*usr));
    usr->uid = 1; usr->gid = 1;
    strncpy(usr->name, "user", USER_NAME_LEN - 1);
    hash_password("", usr->pw_hash);   /* No password by default */
    strncpy(usr->home,  "/home/user", USER_HOME_LEN - 1);
    strncpy(usr->shell, "/bin/ash",   USER_SHELL_LEN - 1);
    usr->active = true;

    users_save();
    kinfo("USERS: created default accounts (system, user)");
}

/* =========================================================
 * Public API
 * ========================================================= */
void users_init(void)
{
    g_count = 0;
    load_db();
    if (g_count == 0) {
        kinfo("USERS: no DB found, creating defaults");
        create_defaults();
    } else {
        kinfo("USERS: loaded %d accounts from DB", g_count);
    }
}

int users_authenticate(const char* name, const char* password)
{
    for (int i = 0; i < g_count; i++) {
        if (!g_users[i].active) continue;
        if (strcmp(g_users[i].name, name) != 0) continue;
        if (g_users[i].locked) return -1;
        if (check_password(&g_users[i], password))
            return (int)g_users[i].uid;
    }
    return -1;
}

const user_t* users_get_by_uid(uint32_t uid)
{
    for (int i = 0; i < g_count; i++)
        if (g_users[i].uid == uid && g_users[i].active)
            return &g_users[i];
    return NULL;
}

const user_t* users_get_by_name(const char* name)
{
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_users[i].name, name) == 0 && g_users[i].active)
            return &g_users[i];
    return NULL;
}

int users_create(const char* name, const char* password,
                  const char* home, const char* shell)
{
    if (g_count >= USERS_MAX) return -1;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_users[i].name, name) == 0) return -1;

    /* Find next uid */
    uint32_t max_uid = 0;
    for (int i = 0; i < g_count; i++)
        if (g_users[i].uid > max_uid) max_uid = g_users[i].uid;

    user_t* u = &g_users[g_count++];
    memset(u, 0, sizeof(*u));
    u->uid = max_uid + 1;
    u->gid = u->uid;
    strncpy(u->name, name, USER_NAME_LEN - 1);
    hash_password(password, u->pw_hash);
    strncpy(u->home,  home  ? home  : "/home/user", USER_HOME_LEN - 1);
    strncpy(u->shell, shell ? shell : "/bin/ash",   USER_SHELL_LEN - 1);
    u->active = true;

    /* Create home directory */
    vfs_mkdir(u->home);
    users_save();
    kinfo("USERS: created user '%s' uid=%u", u->name, u->uid);
    return (int)u->uid;
}

int users_chpasswd(uint32_t uid, const char* old_pw, const char* new_pw)
{
    user_t* u = NULL;
    for (int i = 0; i < g_count; i++)
        if (g_users[i].uid == uid) { u = &g_users[i]; break; }
    if (!u) return -1;

    /* uid 0 can skip old password check */
    if (uid != 0 && !check_password(u, old_pw)) return -1;

    hash_password(new_pw, u->pw_hash);
    users_save();
    return 0;
}

void users_lock(uint32_t uid)
{
    for (int i = 0; i < g_count; i++)
        if (g_users[i].uid == uid) { g_users[i].locked = true; users_save(); return; }
}

void users_unlock(uint32_t uid)
{
    for (int i = 0; i < g_count; i++)
        if (g_users[i].uid == uid) { g_users[i].locked = false; users_save(); return; }
}

int users_count(void) { return g_count; }
const user_t* users_list(void) { return g_users; }
