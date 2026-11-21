/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomRpmh.h"

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;}
static int fits(size_t at, size_t length, size_t size) {return at <= size && length <= size - at;}

/* Header: u32 version, u32 magic, 8 slave records of 16 bytes (u16 id, header offset, data offset, entry count, version, reserved[3]),
 * u32 checksum, u32 reserved, then the data area the offsets count from (byte 144). Entries are 24 bytes: an 8-byte id, two u32
 * priorities, u32 address, u16 aux length, u16 aux offset. */
int qcom_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, QcomRpmhResource *out)
{
    if (!db || !name || !out) return QcomRpmhArgument;
    if (bytes < 144 || le32(db + 4) != QCOM_CMD_DB_MAGIC) return QcomRpmhNoDb;
    char id[8] = {0};
    for (unsigned i = 0; i < 8 && name[i]; ++i) id[i] = name[i];
    for (unsigned s = 0; s < 8; ++s) {
        const uint8_t *slave = db + 8 + 16 * s;
        if (!le16(slave)) break;
        const size_t heads = 144u + le16(slave + 2), data = 144u + le16(slave + 4), count = le16(slave + 6);
        if (!fits(heads, count * 24, bytes)) return QcomRpmhCorrupt;
        for (size_t e = 0; e < count; ++e) {
            const uint8_t *entry = db + heads + 24 * e;
            unsigned same = 1;
            for (unsigned i = 0; i < 8 && same; ++i) same = entry[i] == (uint8_t)id[i];
            if (!same) continue;
            QcomRpmhResource r = {0};
            r.address = le32(entry + 16);
            const size_t length = le16(entry + 20), at = data + le16(entry + 22);
            if (!r.address || !fits(at, length, bytes)) return QcomRpmhCorrupt;
            for (size_t l = 0; l + 1 < length && r.level_count < QCOM_RPMH_MAX_LEVELS; l += 2) {
                const uint16_t level = le16(db + at + l);
                if (r.level_count && !level) break;  /* the list is zero padded */
                r.levels[r.level_count++] = level;
            }
            *out = r;
            return QcomRpmhOk;
        }
    }
    return QcomRpmhMissing;
}

int qcom_rpmh_level_index(const QcomRpmhResource *r, uint32_t level, uint32_t *index)
{
    if (!r->level_count) return QcomRpmhNoLevel;
    if (!level) {*index = 0; return QcomRpmhOk;}
    for (unsigned i = 0; i < r->level_count; ++i)
        if (r->levels[i] >= level) {*index = i; return QcomRpmhOk;}
    *index = r->level_count - 1;
    return QcomRpmhOk;
}

/* rpmh_rsc_reg_offsets_ver_3_0 / _ver_2_7 of rpmh-rsc.c. The id register's bits 23:16 are the major version -- this board's DRV-2
 * reads 0x00030200, i.e. 3.2 -- and version 3 moved the TCS registers up by twelve bytes and widened the command stride to 24. */
QcomRscLayout qcom_rsc_layout(uint32_t drv_id)
{
    if (((drv_id >> 16) & 0xff) >= 3) return (QcomRscLayout){672, 24, 0x00, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40};
    return (QcomRscLayout){672, 20, 0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c};
}

/* CMD_MSGID_LEN, CMD_MSGID_RESP_REQ, CMD_MSGID_WRITE, TCS_AMC_MODE_ENABLE and TCS_AMC_MODE_TRIGGER of rpmh-rsc.c. */
#define MSGID_LEN 8u
#define MSGID_RESP_REQ (1u << 8)
#define MSGID_WRITE (1u << 16)
#define AMC_MODE_ENABLE (1u << 16)
#define AMC_MODE_TRIGGER (1u << 24)

unsigned qcom_rpmh_active_request(const QcomRscLayout *l, unsigned tcs, const uint32_t *addresses, const uint32_t *values, unsigned count,
                                  int wait, QcomRpmhWrite *w, unsigned capacity)
{
    /* The buffer is three writes per command; around it, two to drop the previous transfer's masks, two for the masks of this one and
     * four for the AMC mode sequence of __tcs_set_trigger(). */
    if (!count || count > 4 || capacity < 3 * count + 9) return 0;
    const uint32_t base = l->tcs_stride * tcs, mask = (1u << count) - 1;
    unsigned n = 0;
    /* rpmh_rsc_send_data(): whatever the previous transfer left enabled goes first, so the buffer below is the only thing armed. */
    w[n++] = (QcomRpmhWrite){base + l->cmd_enable, 0, QCOM_RPMH_WHOLE_WORD};
    w[n++] = (QcomRpmhWrite){base + l->wait_for_cmpl, 0, QCOM_RPMH_WHOLE_WORD};
    const uint32_t msgid = MSGID_LEN | MSGID_WRITE | (wait ? MSGID_RESP_REQ : 0);
    for (unsigned c = 0; c < count; ++c) {
        const uint32_t cmd = base + l->cmd_stride * c;
        w[n++] = (QcomRpmhWrite){cmd + l->cmd_msgid, msgid, QCOM_RPMH_WHOLE_WORD};
        w[n++] = (QcomRpmhWrite){cmd + l->cmd_addr, addresses[c], QCOM_RPMH_WHOLE_WORD};
        w[n++] = (QcomRpmhWrite){cmd + l->cmd_data, values[c], QCOM_RPMH_WHOLE_WORD};
    }
    /* __tcs_buffer_write(): the completion mask is written before the commands it applies to. A command that asks for a response is
     * also a command the TCS waits for; the response itself is read out of CMD_STATUS by the owner, never waited on in the RSC. */
    w[n++] = (QcomRpmhWrite){base + l->wait_for_cmpl, wait ? mask : 0, QCOM_RPMH_WHOLE_WORD};
    w[n++] = (QcomRpmhWrite){base + l->cmd_enable, mask, QCOM_RPMH_WHOLE_WORD};
    /* __tcs_set_trigger(true), in the only order the hardware takes: "While clearing ensure that the AMC mode trigger is cleared and
     * then the mode enable is cleared". Each clear touches its own bit; the usb6 boot died writing both away in one store. */
    w[n++] = (QcomRpmhWrite){base + l->control, 0, AMC_MODE_TRIGGER};
    w[n++] = (QcomRpmhWrite){base + l->control, 0, AMC_MODE_ENABLE};
    w[n++] = (QcomRpmhWrite){base + l->control, AMC_MODE_ENABLE, QCOM_RPMH_WHOLE_WORD};
    w[n++] = (QcomRpmhWrite){base + l->control, AMC_MODE_ENABLE | AMC_MODE_TRIGGER, QCOM_RPMH_WHOLE_WORD};
    return n;
}

/* tcs_tx_done(): a TCS is free again once its command-enable and completion masks are clear and its interrupt status is
 * acknowledged. The control register is deliberately left alone -- the next request clears the AMC mode bits itself, in order. */
unsigned qcom_rpmh_finish(const QcomRscLayout *l, unsigned tcs, QcomRpmhWrite *w, unsigned capacity)
{
    if (capacity < 3) return 0;
    const uint32_t base = l->tcs_stride * tcs;
    w[0] = (QcomRpmhWrite){base + l->cmd_enable, 0, QCOM_RPMH_WHOLE_WORD};
    w[1] = (QcomRpmhWrite){base + l->wait_for_cmpl, 0, QCOM_RPMH_WHOLE_WORD};
    w[2] = (QcomRpmhWrite){l->irq_clear, 1u << tcs, QCOM_RPMH_WHOLE_WORD};
    return 3;
}
