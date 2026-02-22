/*
 * include/aether/vec.h — Math primitives for the Aether spatial model
 *
 * All positions and dimensions in ARE use integer pixel coordinates.
 * Opacity uses 0-255. Fractions use 0-256 fixed-point (256 = 1.0).
 * No floating-point required — safe for kernel context.
 */
#pragma once
#include <types.h>

/* =========================================================
 * 2D integer vector
 * ========================================================= */
typedef struct { int32_t x, y; } vec2_t;

static inline vec2_t vec2(int32_t x, int32_t y)
    { return (vec2_t){ x, y }; }
static inline vec2_t vec2_add(vec2_t a, vec2_t b)
    { return (vec2_t){ a.x+b.x, a.y+b.y }; }
static inline vec2_t vec2_sub(vec2_t a, vec2_t b)
    { return (vec2_t){ a.x-b.x, a.y-b.y }; }
static inline vec2_t vec2_lerp(vec2_t a, vec2_t b, int t)
    { return (vec2_t){ a.x + ((b.x-a.x)*t)/256, a.y + ((b.y-a.y)*t)/256 }; }

/* =========================================================
 * ARGB color
 * ========================================================= */
typedef uint32_t acolor_t;

#define ACOLOR(r,g,b,a) \
    (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

#define ACOLOR_R(c) (((c)>>16)&0xFF)
#define ACOLOR_G(c) (((c)>> 8)&0xFF)
#define ACOLOR_B(c) ( (c)     &0xFF)
#define ACOLOR_A(c) (((c)>>24)&0xFF)

/* Blend src over dst using src.alpha */
static inline acolor_t acolor_blend(acolor_t src, acolor_t dst)
{
    uint32_t a = ACOLOR_A(src);
    uint32_t ia = 255 - a;
    uint8_t r = (uint8_t)((ACOLOR_R(src)*a + ACOLOR_R(dst)*ia) / 255);
    uint8_t g = (uint8_t)((ACOLOR_G(src)*a + ACOLOR_G(dst)*ia) / 255);
    uint8_t b = (uint8_t)((ACOLOR_B(src)*a + ACOLOR_B(dst)*ia) / 255);
    return ACOLOR(r, g, b, 0xFF);
}

/* =========================================================
 * Integer rectangle
 * ========================================================= */
typedef struct { int32_t x, y, w, h; } rect_t;

static inline rect_t rect(int x, int y, int w, int h)
    { return (rect_t){ x, y, w, h }; }
static inline bool rect_contains(rect_t r, int x, int y)
    { return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h; }

/* =========================================================
 * Easing — ease-out cubic (0→64 input, 0→255 output)
 * Table generated from: round(255 * (1 - (1 - t/64)^3))
 * ========================================================= */
extern const uint8_t EASE_OUT_64[65];

/* Linear interpolate (t: 0-256) */
static inline int lrp(int a, int b, int t)
    { return a + ((b - a) * t) / 256; }

/* Map frame f in [0..total] to 0..255 eased progress */
static inline uint8_t ease_progress(int f, int total)
{
    if (total <= 0 || f >= total) return 255;
    int idx = (f * 64) / total;
    return EASE_OUT_64[idx < 64 ? idx : 64];
}
