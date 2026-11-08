/* SPDX-License-Identifier: MIT
 *
 * QR encoding for the panel diagnostics: byte mode, level L, versions 1-3 (single block at L, so no
 * interleaving). The point is a channel that cannot be mistyped - the host photographs the panel and
 * the data arrives as decoded bytes instead of hand-copied text.
 *
 * Verified against qrencode 4.1.1 on the host: for nine texts spanning all three versions, one of the
 * eight masks reproduces qrencode's matrix module for module, which pins the Reed-Solomon codewords,
 * the placement, the format information and the masking. See docs/STATE.md section 84.
 */
#ifndef QCOM_USB_QR_H
#define QCOM_USB_QR_H

#include <stddef.h>
#include <stdint.h>

#define QR_MAX_VERSION  5
#define QR_MAX_SIZE     37                /* 17 + 4 * 5 */
#define QR_MAX_CODEPTS  140               /* version 5-L: 108 data + 26 ecc */

struct qr_code {
    uint8_t size;                         /* modules per side: 21, 25 or 29 */
    uint8_t version;
    uint8_t mask;
    uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE];   /* 1 = dark, row major */
};

static const uint8_t qr_ecc_length[5] = { 7, 10, 15, 20, 26 };  /* versions 1..5, level L */
static const uint8_t qr_codewords[5]  = { 26, 44, 70, 100, 134 };  /* total codewords, versions 1..5 */

static uint8_t qr_gf_exp[512];
static uint8_t qr_gf_log[256];
static int qr_gf_ready;

static void qr_gf_init(void)
{
    unsigned i, x = 1;
    if (qr_gf_ready) return;
    for (i = 0; i < 255; ++i) {
        qr_gf_exp[i] = (uint8_t)x;
        qr_gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u) x ^= 0x11du;
    }
    for (i = 255; i < 512; ++i) qr_gf_exp[i] = qr_gf_exp[i - 255];
    qr_gf_ready = 1;
}

static uint8_t qr_gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    return qr_gf_exp[qr_gf_log[a] + qr_gf_log[b]];
}

/* Generator polynomials for the three block sizes at level L, highest coefficient first. */
static void qr_ecc(const uint8_t *data, unsigned length, unsigned count, uint8_t *out)
{
    static const uint8_t gen7[7]   = { 127, 122, 154, 164, 11, 68, 117 };
    static const uint8_t gen10[10] = { 216, 194, 159, 111, 199, 94, 95, 113, 157, 193 };
    static const uint8_t gen15[15] = { 29, 196, 111, 163, 112, 74, 10, 105, 105, 139, 132, 151, 32, 134, 26 };
    static const uint8_t gen20[20] = { 152, 185, 240, 5, 111, 99, 6, 220, 112, 150, 69, 36, 187, 22, 228, 198, 121, 121, 165, 174 };
    static const uint8_t gen26[26] = { 246, 51, 183, 4, 136, 98, 199, 152, 77, 56, 206, 24, 145, 40, 209, 117, 233, 42, 135, 68, 70, 144, 146, 77, 43, 94 };
    const uint8_t *gen = count == 7 ? gen7 : count == 10 ? gen10 : count == 15 ? gen15 :
                         count == 20 ? gen20 : gen26;
    uint8_t rem[32];
    unsigned i, j;

    for (i = 0; i < count; ++i) rem[i] = 0;
    for (i = 0; i < length; ++i) {
        uint8_t factor = (uint8_t)(data[i] ^ rem[0]);
        for (j = 0; j + 1 < count; ++j) rem[j] = rem[j + 1];
        rem[count - 1] = 0;
        for (j = 0; j < count; ++j) rem[j] ^= qr_gf_mul(gen[j], factor);
    }
    for (i = 0; i < count; ++i) out[i] = rem[i];
}

static void qr_set(struct qr_code *qr, int row, int col, int dark)
{
    qr->modules[row * qr->size + col] = dark ? 1 : 0;
}

static uint8_t qr_get(const struct qr_code *qr, int row, int col)
{
    return qr->modules[row * qr->size + col];
}

/* Function-pattern map: 1 where a module belongs to a finder, separator, timing, alignment or the
 * format/dark area, i.e. where data must not be placed and masking must not apply. */
static void qr_reserve(struct qr_code *qr, uint8_t *reserved)
{
    int i, r, c, center;
    for (i = 0; i < QR_MAX_SIZE * QR_MAX_SIZE; ++i) reserved[i] = 0;
    for (r = 0; r <= 8; ++r) for (c = 0; c <= 8; ++c) reserved[r * qr->size + c] = 1;
    for (r = 0; r <= 8; ++r) for (c = qr->size - 8; c < qr->size; ++c) reserved[r * qr->size + c] = 1;
    for (r = qr->size - 8; r < qr->size; ++r) for (c = 0; c <= 8; ++c) reserved[r * qr->size + c] = 1;
    for (i = 0; i < qr->size; ++i) {
        reserved[6 * qr->size + i] = 1;
        reserved[i * qr->size + 6] = 1;
    }
    if (qr->version >= 2) {
        center = qr->size - 7;                 /* 18,22,26,30 for versions 2..5 */
        for (r = center - 2; r <= center + 2; ++r)
            for (c = center - 2; c <= center + 2; ++c) reserved[r * qr->size + c] = 1;
    }
}

static void qr_function_patterns(struct qr_code *qr)
{
    int i, r, c, center;

    for (i = 0; i < QR_MAX_SIZE * QR_MAX_SIZE; ++i) qr->modules[i] = 0;

    /* three finders with their separators */
    for (r = 0; r < 9; ++r) {
        for (c = 0; c < 9; ++c) {
            int ring = (r == 0 || r == 6 || c == 0 || c == 6) && r <= 6 && c <= 6;
            int core = r >= 2 && r <= 4 && c >= 2 && c <= 4;
            if (r <= 7 && c <= 7) qr_set(qr, r, c, ring || core);
            if (r <= 7) qr_set(qr, r, qr->size - 1 - c, ring || core);
            if (c <= 7) qr_set(qr, qr->size - 1 - r, c, ring || core);
        }
    }
    for (i = 8; i < qr->size - 8; ++i) {
        qr_set(qr, 6, i, i % 2 == 0);
        qr_set(qr, i, 6, i % 2 == 0);
    }
    if (qr->version >= 2) {
        center = qr->size - 7;
        for (r = -2; r <= 2; ++r) {
            for (c = -2; c <= 2; ++c) {
                int ring = (r == -2 || r == 2 || c == -2 || c == 2);
                qr_set(qr, center + r, center + c, ring || (r == 0 && c == 0));
            }
        }
    }
}

/* 15 format bits: two level bits, three mask bits, BCH(15,5) remainder, XOR 0x5412. */
static uint32_t qr_format_bits(unsigned mask)
{
    uint32_t data = (uint32_t)((1u << 3) | (mask & 7u));   /* level L is 01 */
    uint32_t rem = data, bits;
    int i;
    for (i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
    bits = ((data << 10) | (rem & 0x3ffu)) ^ 0x5412u;
    return bits;
}

static void qr_draw_format(struct qr_code *qr)
{
    uint32_t bits = qr_format_bits(qr->mask);
    int i;
    for (i = 0; i <= 5; ++i) qr_set(qr, i, 8, (int)((bits >> i) & 1u));
    qr_set(qr, 7, 8, (int)((bits >> 6) & 1u));
    qr_set(qr, 8, 8, (int)((bits >> 7) & 1u));
    qr_set(qr, 8, 7, (int)((bits >> 8) & 1u));
    for (i = 9; i < 15; ++i) qr_set(qr, 8, 14 - i, (int)((bits >> i) & 1u));
    for (i = 0; i < 8; ++i) qr_set(qr, 8, qr->size - 1 - i, (int)((bits >> i) & 1u));
    for (i = 8; i < 15; ++i) qr_set(qr, qr->size - 15 + i, 8, (int)((bits >> i) & 1u));
    qr_set(qr, qr->size - 8, 8, 1);
}

static void qr_place_data(struct qr_code *qr, const uint8_t *codewords, unsigned count,
                          const uint8_t *reserved)
{
    unsigned bit = 0, total = count * 8u;
    int right, vert, j;

    for (right = qr->size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (vert = 0; vert < qr->size; ++vert) {
            for (j = 0; j < 2; ++j) {
                int col = right - j;
                int upward = ((right + 1) & 2) == 0;
                int row = upward ? qr->size - 1 - vert : vert;
                uint8_t dark;
                if (reserved[row * qr->size + col]) continue;
                dark = bit < total ? (uint8_t)((codewords[bit >> 3] >> (7 - (bit & 7))) & 1u) : 0;
                qr_set(qr, row, col, dark);
                ++bit;
            }
        }
    }
}

static int qr_mask_applies(unsigned mask, int row, int col)
{
    switch (mask) {
    case 0: return (row + col) % 2 == 0;
    case 1: return row % 2 == 0;
    case 2: return col % 3 == 0;
    case 3: return (row + col) % 3 == 0;
    case 4: return (row / 2 + col / 3) % 2 == 0;
    case 5: return (row * col) % 2 + (row * col) % 3 == 0;
    case 6: return ((row * col) % 2 + (row * col) % 3) % 2 == 0;
    default: return ((row + col) % 2 + (row * col) % 3) % 2 == 0;
    }
}

static void qr_apply_mask(struct qr_code *qr, const uint8_t *reserved, unsigned mask)
{
    int row, col;
    for (row = 0; row < qr->size; ++row) {
        for (col = 0; col < qr->size; ++col) {
            unsigned at = (unsigned)(row * qr->size + col);
            if (reserved[at]) continue;
            if (qr_mask_applies(mask, row, col)) qr->modules[at] ^= 1u;
        }
    }
}

static long qr_penalty(const struct qr_code *qr)
{
    static const uint8_t finder_a[7] = { 1, 0, 1, 1, 1, 0, 1 };
    long score = 0, dark = 0;
    int row, col, i, run;
    uint8_t prev;

    for (row = 0; row < qr->size; ++row) {
        run = 1; prev = qr_get(qr, row, 0);
        for (col = 1; col < qr->size; ++col) {
            uint8_t v = qr_get(qr, row, col);
            if (v == prev) { if (++run == 5) score += 3; else if (run > 5) ++score; }
            else { run = 1; prev = v; }
        }
    }
    for (col = 0; col < qr->size; ++col) {
        run = 1; prev = qr_get(qr, 0, col);
        for (row = 1; row < qr->size; ++row) {
            uint8_t v = qr_get(qr, row, col);
            if (v == prev) { if (++run == 5) score += 3; else if (run > 5) ++score; }
            else { run = 1; prev = v; }
        }
    }
    for (row = 0; row + 1 < qr->size; ++row) {
        for (col = 0; col + 1 < qr->size; ++col) {
            uint8_t v = qr_get(qr, row, col);
            if (v == qr_get(qr, row, col + 1) && v == qr_get(qr, row + 1, col) &&
                v == qr_get(qr, row + 1, col + 1)) score += 3;
        }
    }
    /* 1:0:1:1:1:0:1 with four light modules on one side */
    for (row = 0; row < qr->size; ++row) {
        for (col = 0; col + 11 <= qr->size; ++col) {
            int match = 1;
            for (i = 0; i < 7; ++i) if (qr_get(qr, row, col + 1 + i) != finder_a[i]) match = 0;
            if (match && qr_get(qr, row, col) == 0 &&
                qr_get(qr, row, col + 8) == 0 && qr_get(qr, row, col + 9) == 0 &&
                qr_get(qr, row, col + 10) == 0) score += 40;
        }
    }
    for (col = 0; col < qr->size; ++col) {
        for (row = 0; row + 11 <= qr->size; ++row) {
            int match = 1;
            for (i = 0; i < 7; ++i) if (qr_get(qr, row + 1 + i, col) != finder_a[i]) match = 0;
            if (match && qr_get(qr, row, col) == 0 &&
                qr_get(qr, row + 8, col) == 0 && qr_get(qr, row + 9, col) == 0 &&
                qr_get(qr, row + 10, col) == 0) score += 40;
        }
    }
    for (row = 0; row < qr->size; ++row)
        for (col = 0; col < qr->size; ++col) dark += qr_get(qr, row, col);
    {
        long total = (long)qr->size * qr->size;
        long percent = dark * 100 / total;
        long dev = percent > 50 ? percent - 50 : 50 - percent;
        score += (dev / 5) * 10;
    }
    return score;
}

/* Encode text into one symbol; forced_mask >= 0 pins the mask (used by the host verification). */
static int qr_encode_mask(const char *text, struct qr_code *qr, int forced_mask)
{
    uint8_t codewords[QR_MAX_CODEPTS];
    uint8_t reserved[QR_MAX_SIZE * QR_MAX_SIZE];
    uint8_t plain[QR_MAX_SIZE * QR_MAX_SIZE];
    uint8_t best[QR_MAX_SIZE * QR_MAX_SIZE];
    unsigned length = 0, ecc_count = 0, total = 0, data_count = 0, at = 0, bit = 0, i;
    long best_score = -1;
    int version, mask;

    qr_gf_init();
    while (text[length] != '\0') ++length;
    if (length > 106) return 0;         /* version 5-L byte capacity: (108*8 - 12) / 8 */

    for (version = 1; version <= QR_MAX_VERSION; ++version) {
        qr->size = (uint8_t)(17 + 4 * version);
        qr->version = (uint8_t)version;
        ecc_count = qr_ecc_length[version - 1];
        total = qr_codewords[version - 1];
        data_count = total - ecc_count;
        if (4u + 8u + length * 8u <= data_count * 8u) break;
    }
    if (version > QR_MAX_VERSION) return 0;

    for (i = 0; i < sizeof(codewords); ++i) codewords[i] = 0;
    for (i = 0; i < 4u; ++i, ++at) if ((0x4u >> (3u - i)) & 1u) codewords[at >> 3] |= (uint8_t)(0x80u >> (at & 7u));
    for (i = 0; i < 8u; ++i, ++at) if ((length >> (7u - i)) & 1u) codewords[at >> 3] |= (uint8_t)(0x80u >> (at & 7u));
    for (i = 0; i < length; ++i) {
        unsigned k;
        for (k = 0; k < 8u; ++k, ++at)
            if (((unsigned char)text[i] >> (7u - k)) & 1u) codewords[at >> 3] |= (uint8_t)(0x80u >> (at & 7u));
    }
    bit = at;
    if (bit + 4u <= data_count * 8u) bit += 4u;            /* terminator */
    bit = (bit + 7u) & ~7u;
    for (i = bit >> 3; i < data_count; ++i)
        codewords[i] = ((i - (bit >> 3)) & 1u) ? 0x11u : 0xECu;
    qr_ecc(codewords, data_count, ecc_count, codewords + data_count);

    qr_function_patterns(qr);
    qr_reserve(qr, reserved);
    qr_place_data(qr, codewords, total, reserved);
    for (i = 0; i < sizeof(plain); ++i) plain[i] = qr->modules[i];

    for (mask = 0; mask < 8; ++mask) {
        long score;
        if (forced_mask >= 0 && mask != forced_mask) continue;
        for (i = 0; i < sizeof(plain); ++i) qr->modules[i] = plain[i];
        qr_apply_mask(qr, reserved, (unsigned)mask);
        qr->mask = (uint8_t)mask;
        qr_draw_format(qr);
        score = qr_penalty(qr);
        if (best_score < 0 || score < best_score) {
            best_score = score;
            for (i = 0; i < sizeof(plain); ++i) best[i] = qr->modules[i];
        }
    }
    for (i = 0; i < sizeof(plain); ++i) qr->modules[i] = best[i];
    return 1;
}

static int qr_encode(const char *text, struct qr_code *qr)
{
    return qr_encode_mask(text, qr, -1);
}

#endif /* QCOM_USB_QR_H */
