/*
 * types.h - Fundamental type definitions for the kernel
 *
 * This header provides platform-independent type aliases used
 * throughout the kernel. All sizes are fixed-width for x86_64.
 */
#ifndef TYPES_H
#define TYPES_H

/* Unsigned integer types */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

/* Signed integer types */
typedef signed char      int8_t;
typedef signed short     int16_t;
typedef signed int       int32_t;
typedef signed long long int64_t;

/* Size/pointer types */
typedef uint64_t size_t;
typedef int64_t  ssize_t;
typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;
typedef uint64_t off_t;
typedef uint32_t pid_t;
typedef int32_t  tid_t;

/* Boolean type */
typedef int bool;
#define true  1
#define false 0

/* NULL */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Limits */
#define UINT8_MAX  0xFF
#define UINT16_MAX 0xFFFF
#define UINT32_MAX 0xFFFFFFFF
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#define INT32_MAX  0x7FFFFFFF
#define INT64_MAX  0x7FFFFFFFFFFFFFFFLL

/* Compiler attributes */
#define PACKED      __attribute__((packed))
#define ALIGN(n)    __attribute__((aligned(n)))
#define NORETURN    __attribute__((noreturn))
#define UNUSED      __attribute__((unused))
#define ALWAYS_INLINE __attribute__((always_inline))
#define NOINLINE    __attribute__((noinline))

/* offsetof - byte offset of a member within a struct */
#ifndef offsetof
#define offsetof(type, member)  __builtin_offsetof(type, member)
#endif

/* Utility macros */
#define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, align)  (((x) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ABS(x)              ((x) < 0 ? -(x) : (x))
#define BIT(n)              (1ULL << (n))
#define BITS(hi, lo)        (((1ULL << ((hi) - (lo) + 1)) - 1) << (lo))

/* Kernel virtual base address */
#define KERNEL_VMA_BASE     0xFFFFFFFF80000000ULL
#define KERNEL_PHYS_BASE    0x100000ULL

/* Convert between physical and kernel virtual addresses */
#define PHYS_TO_VIRT(addr)  ((void*)((uint64_t)(addr) + KERNEL_VMA_BASE))
#define VIRT_TO_PHYS(addr)  ((uint64_t)(addr) - KERNEL_VMA_BASE)

/* Page size */
#define PAGE_SIZE           4096
#define PAGE_SHIFT          12
#define PAGE_MASK           (~(PAGE_SIZE - 1))

#endif /* TYPES_H */
