/*
 * test_bitstream.c - unit tests for the bit writer and Exp-Golomb coding
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "common/bitstream.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* A minimal MSB-first reader, used to round-trip the writer. */
typedef struct { const uint8_t *p; const uint8_t *end; int bit; } reader_t;

static int rd_bit(reader_t *r)
{
    if (r->p >= r->end)
        return 0;
    int b = (*r->p >> (7 - r->bit)) & 1;
    if (++r->bit == 8) { r->bit = 0; r->p++; }
    return b;
}

static uint32_t rd_bits(reader_t *r, int n)
{
    uint32_t v = 0;
    while (n--)
        v = (v << 1) | (uint32_t)rd_bit(r);
    return v;
}

static uint32_t rd_ue(reader_t *r)
{
    int zeros = 0;
    while (rd_bit(r) == 0 && r->p < r->end)
        zeros++;
    uint32_t rest = rd_bits(r, zeros);
    return ((1u << zeros) | rest) - 1;
}

static int32_t rd_se(reader_t *r)
{
    uint32_t code = rd_ue(r);
    uint32_t k = (code + 1) / 2;
    return (code & 1) ? (int32_t)k : -(int32_t)k;
}

static void test_fixed(void)
{
    uint8_t buf[16];
    n264_bs_t bs;

    n264_bs_init(&bs, buf, sizeof(buf));
    n264_bs_write1(&bs, 1);
    n264_bs_flush(&bs);
    CHECK(buf[0] == 0x80, "write1(1) => 0x%02x", buf[0]);

    n264_bs_init(&bs, buf, sizeof(buf));
    n264_bs_write_ue(&bs, 0);
    n264_bs_flush(&bs);
    CHECK(buf[0] == 0x80, "ue(0) => 0x%02x", buf[0]);

    n264_bs_init(&bs, buf, sizeof(buf));
    n264_bs_write_ue(&bs, 1);
    n264_bs_flush(&bs);
    CHECK(buf[0] == 0x40, "ue(1) => 0x%02x", buf[0]);   /* 010 */

    n264_bs_init(&bs, buf, sizeof(buf));
    n264_bs_write_ue(&bs, 3);
    n264_bs_flush(&bs);
    CHECK(buf[0] == 0x20, "ue(3) => 0x%02x", buf[0]);   /* 00100 */

    n264_bs_init(&bs, buf, sizeof(buf));
    n264_bs_write(&bs, 8, 0xA5);
    n264_bs_flush(&bs);
    CHECK(buf[0] == 0xA5, "write(8,0xA5) => 0x%02x", buf[0]);

    /* write must mask stray high bits. */
    n264_bs_init(&bs, buf, sizeof(buf));
    n264_bs_write(&bs, 4, 0xFF);
    n264_bs_flush(&bs);
    CHECK(buf[0] == 0xF0, "write(4,0xFF) => 0x%02x", buf[0]);
}

static void test_roundtrip_ue(void)
{
    uint8_t buf[64];
    n264_bs_t bs;
    uint32_t vals[] = { 0, 1, 2, 3, 4, 7, 8, 100, 1000, 65535, 100000, 1u << 20 };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        n264_bs_init(&bs, buf, sizeof(buf));
        n264_bs_write_ue(&bs, vals[i]);
        n264_bs_rbsp_trailing(&bs);
        CHECK(!bs.overflow, "ue(%u) overflow", vals[i]);
        reader_t r = { buf, bs.p, 0 };
        uint32_t got = rd_ue(&r);
        CHECK(got == vals[i], "ue roundtrip: wrote %u read %u", vals[i], got);
    }
}

static void test_roundtrip_se(void)
{
    uint8_t buf[64];
    n264_bs_t bs;
    int32_t vals[] = { 0, 1, -1, 2, -2, 26, -26, 1000, -1000, 32767, -32768 };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        n264_bs_init(&bs, buf, sizeof(buf));
        n264_bs_write_se(&bs, vals[i]);
        n264_bs_rbsp_trailing(&bs);
        CHECK(!bs.overflow, "se(%d) overflow", vals[i]);
        reader_t r = { buf, bs.p, 0 };
        int32_t got = rd_se(&r);
        CHECK(got == vals[i], "se roundtrip: wrote %d read %d", vals[i], got);
    }
}

static void test_mixed_stream(void)
{
    /* Interleave field widths and check the whole sequence reads back. */
    uint8_t buf[64];
    n264_bs_t bs;
    n264_bs_init(&bs, buf, sizeof(buf));
    n264_bs_write(&bs, 8, 66);        /* profile_idc-like */
    n264_bs_write(&bs, 8, 0);
    n264_bs_write(&bs, 8, 51);
    n264_bs_write_ue(&bs, 0);
    n264_bs_write_ue(&bs, 119);       /* width_in_mbs-1 for 1920 */
    n264_bs_write_ue(&bs, 67);        /* height_in_map_units-1 for 1088 */
    n264_bs_write1(&bs, 1);
    n264_bs_write_se(&bs, -3);
    n264_bs_rbsp_trailing(&bs);
    CHECK(!bs.overflow, "mixed stream overflow");

    reader_t r = { buf, bs.p, 0 };
    CHECK(rd_bits(&r, 8) == 66, "mixed: profile");
    CHECK(rd_bits(&r, 8) == 0, "mixed: constraints");
    CHECK(rd_bits(&r, 8) == 51, "mixed: level");
    CHECK(rd_ue(&r) == 0, "mixed: sps_id");
    CHECK(rd_ue(&r) == 119, "mixed: width");
    CHECK(rd_ue(&r) == 67, "mixed: height");
    CHECK(rd_bit(&r) == 1, "mixed: flag");
    CHECK(rd_se(&r) == -3, "mixed: se");
}

static void test_overflow(void)
{
    uint8_t buf[2];
    n264_bs_t bs;
    n264_bs_init(&bs, buf, sizeof(buf));
    for (int i = 0; i < 100; i++)
        n264_bs_write(&bs, 8, 0xFF);
    CHECK(bs.overflow, "overflow flag not set on tiny buffer");
}

int main(void)
{
    test_fixed();
    test_roundtrip_ue();
    test_roundtrip_se();
    test_mixed_stream();
    test_overflow();
    if (fails) {
        printf("test_bitstream: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_bitstream: all passed\n");
    return 0;
}
