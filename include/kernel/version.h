/*
 * include/kernel/version.h — AetherOS Centralized Branding & Version Constants
 *
 * This is the single source of truth for all OS identity strings.
 * Every subsystem, application, and service MUST import from here.
 * No OS name, version, or copyright string may be hardcoded elsewhere.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  AetherOS v1.0.0 — "Genesis"
 * ─────────────────────────────────────────────────────────────────────────────
 */
#ifndef KERNEL_VERSION_H
#define KERNEL_VERSION_H

/* ── Product identity ────────────────────────────────────────────────────── */
#define OS_NAME          "AetherOS"
#define OS_SHORT_NAME    "Aether"
#define OS_HOSTNAME      "aetheros"

/* ── Version ─────────────────────────────────────────────────────────────── */
#define OS_VERSION_MAJOR  1
#define OS_VERSION_MINOR  0
#define OS_VERSION_PATCH  0
#define OS_VERSION        "1.0.0"
#define OS_RELEASE        "Genesis"

/* ── Composed display strings ────────────────────────────────────────────── */
#define OS_BANNER         OS_NAME " v" OS_VERSION " — " OS_RELEASE
#define OS_BANNER_SHORT   OS_NAME " v" OS_VERSION
#define OS_TAGLINE        "Services. Isolation. Trust."
#define OS_ARCH           "x86_64"
#define OS_KERNEL_TYPE    "Hybrid Microkernel"

/* ── Copyright & project ─────────────────────────────────────────────────── */
#define OS_YEAR           "2026"
#define OS_COPYRIGHT      "© " OS_YEAR " AetherOS Project"
#define OS_LICENSE        "Open Source"
#define OS_PROJECT        "AetherOS Project"
#define OS_AUTHOR         "AetherOS Project"

/* ── Service bus names (must match kernel/svcbus.h) ─────────────────────── */
#define OS_SVC_DISPLAY    "aether.display"
#define OS_SVC_INPUT      "aether.input"
#define OS_SVC_VFS        "aether.vfs"
#define OS_SVC_NETWORK    "aether.network"
#define OS_SVC_AUDIO      "aether.audio"
#define OS_SVC_LAUNCHER   "aether.launcher"

/* ── Terminal identity ───────────────────────────────────────────────────── */
#define OS_TERM_NAME      "aether-term"
#define OS_TERM_BANNER    OS_NAME " Terminal  v" OS_VERSION " — " OS_RELEASE

/* ── Boot sequence messages ──────────────────────────────────────────────── */
#define OS_BOOT_LINE1     OS_NAME
#define OS_BOOT_LINE2     "Version " OS_VERSION " — " OS_RELEASE
#define OS_BOOT_LINE3     "Initializing Core Systems..."
#define OS_BOOT_WELCOME   "Welcome to " OS_NAME

/* ── uname(2) fields ─────────────────────────────────────────────────────── */
#define UNAME_SYSNAME     OS_NAME
#define UNAME_NODENAME    OS_HOSTNAME
#define UNAME_RELEASE     OS_VERSION
#define UNAME_VERSION     OS_RELEASE " " OS_YEAR
#define UNAME_MACHINE     OS_ARCH

/* ── Package manager identity ────────────────────────────────────────────── */
#define PKG_MANAGER_NAME  "APM"          /* AetherOS Package Manager */
#define PKG_EXT           ".aur"

/* ── Version integer for compile-time comparisons ────────────────────────── */
#define OS_VERSION_INT    ((OS_VERSION_MAJOR * 10000) + \
                           (OS_VERSION_MINOR * 100)   + \
                           (OS_VERSION_PATCH))

/* ── Macro: emit formatted version string into a buffer ──────────────────── */
#define OS_VERSION_STRING(buf, sz) \
    snprintf((buf), (sz), "%s v%s (%s)", OS_NAME, OS_VERSION, OS_RELEASE)

#endif /* KERNEL_VERSION_H */
