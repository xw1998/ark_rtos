/*
 * ark_bus_util.c - the handful of libc-shaped helpers the core needs, kept
 * local so the portable core pulls in no external runtime.
 */
#include "ark_bus_priv.h"

void *ark_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *ark_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int ark_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

size_t ark_strlen(const char *s)
{
    size_t n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

int ark_strncmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        if (*a != *b) {
            return (int)(uint8_t)*a - (int)(uint8_t)*b;
        }
        if (*a == '\0') {
            return 0;
        }
        a++;
        b++;
    }
    return 0;
}

void ark_strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    if (!dst || n == 0) {
        return;
    }
    if (src) {
        for (; i + 1 < n && src[i]; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

uint16_t ark_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t ark_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

void ark_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

void ark_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFFu);
}
