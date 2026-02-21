/*
 * kernel.h - Core kernel definitions and interfaces
 *
 * This header is included by virtually all kernel components and
 * provides the main kernel API: logging, panic, and utility functions.
 */
#ifndef KERNEL_H
#define KERNEL_H

#include <types.h>
#include <string.h>

/* Kernel version information */
#define KERNEL_NAME    "NovOS"
#define KERNEL_VERSION "0.1.0"
#define KERNEL_ARCH    "x86_64"

/* Log levels */
#define LOG_DEBUG   0
#define LOG_INFO    1
#define LOG_WARN    2
#define LOG_ERROR   3
#define LOG_PANIC   4

/* Current minimum log level (set to DEBUG for development) */
#define LOG_LEVEL LOG_DEBUG

/* Logging macros */
#define klog(level, fmt, ...) kernel_log(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define kdebug(fmt, ...)      klog(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define kinfo(fmt, ...)       klog(LOG_INFO,  fmt, ##__VA_ARGS__)
#define kwarn(fmt, ...)       klog(LOG_WARN,  fmt, ##__VA_ARGS__)
#define kerror(fmt, ...)      klog(LOG_ERROR, fmt, ##__VA_ARGS__)

/* Kernel panic - prints message and halts the system */
#define kpanic(fmt, ...) kernel_panic(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* Assertion macro */
#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            kpanic("Assertion failed: " #cond); \
        } \
    } while(0)

/* Kernel initialization state */
typedef enum {
    KERNEL_STATE_BOOT = 0,
    KERNEL_STATE_INIT,
    KERNEL_STATE_RUNNING,
    KERNEL_STATE_PANIC
} kernel_state_t;

/* Kernel API */
void kernel_log(int level, const char* file, int line, const char* fmt, ...);
void NORETURN kernel_panic(const char* file, int line, const char* fmt, ...);
void kernel_dump_stack_trace(void);
kernel_state_t kernel_get_state(void);
void kernel_set_state(kernel_state_t state);

/* CPU control */
static inline void cpu_halt(void) {
    __asm__ volatile("hlt");
}

static inline void cpu_cli(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void cpu_sti(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline void cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

static inline uint64_t cpu_flags(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static inline bool cpu_interrupts_enabled(void) {
    return (cpu_flags() & (1 << 9)) != 0;
}

/* I/O port access */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* MSR access */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

/* CR register access */
static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("movq %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("movq %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("movq %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    __asm__ volatile("movq %0, %%cr3" :: "r"(val) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("movq %%cr4, %0" : "=r"(val));
    return val;
}

/* TLB flush */
static inline void tlb_flush_page(uint64_t vaddr) {
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

static inline void tlb_flush_all(void) {
    write_cr3(read_cr3());
}

/* RDTSC */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif /* KERNEL_H */
