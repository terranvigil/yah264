/*
 * test_nal.c - unit tests for NAL packaging and emulation prevention
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "common/nal.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static int eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

static void test_passthrough(void)
{
    uint8_t rbsp[] = { 0x11, 0x22, 0x33 };
    uint8_t out[32];
    size_t n = n264_nal_write(out, sizeof(out), 3, 7, rbsp, sizeof(rbsp));
    uint8_t want[] = { 0x00, 0x00, 0x00, 0x01, /* hdr */ 0x67, 0x11, 0x22, 0x33 };
    CHECK(n == sizeof(want), "passthrough size %zu", n);
    CHECK(eq(out, want, n), "passthrough bytes");
    /* header: forbidden_zero_bit=0, nal_ref_idc=3, type=7 => (3<<5)|7 = 0x67 */
}

static void test_emulation_000000(void)
{
    uint8_t rbsp[] = { 0x00, 0x00, 0x00 };
    uint8_t out[32];
    size_t n = n264_nal_write(out, sizeof(out), 0, 1, rbsp, sizeof(rbsp));
    /* header (0<<5)|1 = 0x01; after two zeros a 0x03 is inserted before 0x00 */
    uint8_t want[] = { 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00 };
    CHECK(n == sizeof(want), "emu 000000 size %zu", n);
    CHECK(eq(out, want, n), "emu 000000 bytes");
}

static void test_emulation_000001(void)
{
    uint8_t rbsp[] = { 0x00, 0x00, 0x01 };
    uint8_t out[32];
    size_t n = n264_nal_write(out, sizeof(out), 0, 1, rbsp, sizeof(rbsp));
    uint8_t want[] = { 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x03, 0x01 };
    CHECK(n == sizeof(want), "emu 000001 size %zu", n);
    CHECK(eq(out, want, n), "emu 000001 bytes");
}

static void test_emulation_boundary(void)
{
    /* 0x02 and 0x03 also trigger insertion; 0x04 does not. */
    uint8_t rbsp[] = { 0x00, 0x00, 0x02, 0x00, 0x00, 0x04 };
    uint8_t out[32];
    size_t n = n264_nal_write(out, sizeof(out), 0, 1, rbsp, sizeof(rbsp));
    uint8_t want[] = { 0x00, 0x00, 0x00, 0x01, 0x01,
                       0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x04 };
    CHECK(n == sizeof(want), "emu boundary size %zu", n);
    CHECK(eq(out, want, n), "emu boundary bytes");
}

static void test_too_small(void)
{
    uint8_t rbsp[] = { 0x11, 0x22, 0x33 };
    uint8_t out[4];
    size_t n = n264_nal_write(out, sizeof(out), 3, 7, rbsp, sizeof(rbsp));
    CHECK(n == 0, "too-small buffer should return 0, got %zu", n);
}

int main(void)
{
    test_passthrough();
    test_emulation_000000();
    test_emulation_000001();
    test_emulation_boundary();
    test_too_small();
    if (fails) {
        printf("test_nal: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_nal: all passed\n");
    return 0;
}
