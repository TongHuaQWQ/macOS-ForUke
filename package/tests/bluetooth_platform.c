/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host register-model verification for SM7675 UART10 RCG, apps-SMMU bypass (SID 0x423),
 * and WCN6750 firmware filename validation. */
#include "Sm7675GeniUart.h"
#include "SmmuBypass.h"
#include "BluetoothFirmware.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct model {
    uint32_t regs[0xd0000 / 4];
    unsigned writes, delays;
    bool clock_stuck, dfs_stuck;
};

static uint32_t rd_clk(void *p, uint32_t r) {
    struct model *m = p;
    assert(r < sizeof(m->regs));
    if (r == SM7675_GCC_QUP1_S2_CMD_RCGR && !m->clock_stuck) m->regs[r / 4] &= ~1u;
    return m->regs[r / 4];
}

static void wr_clk(void *p, uint32_t r, uint32_t v) {
    struct model *m = p;
    assert(r < sizeof(m->regs));
    m->writes++;
    if (r == SM7675_GCC_QUP1_S2_CMD_RCGR + 0x14 && m->dfs_stuck) return;
    m->regs[r / 4] = v;
}

static void delay_us(void *p, unsigned us) {
    struct model *m = p;
    assert(us == 1);
    m->delays++;
}

static uint32_t rd_smmu(void *p, unsigned r) {
    struct model *m = p;
    assert(r < sizeof(m->regs));
    return m->regs[r / 4];
}

static void wr_smmu(void *p, unsigned r, uint32_t v) {
    struct model *m = p;
    assert(r < sizeof(m->regs));
    m->writes++;
    m->regs[r / 4] = v;
}

static void init_smmu(struct model *m) {
    memset(m, 0, sizeof(*m));
    /* IDR0: S1TS=1 (bit 30), NUMSMRG=16; IDR1: NUMPAGENDXB=3 (16 pages -> pages=32? shift 28: 1<<(3+1)=16), NUMCB=8; IDR2: OAS=IAS=UBS=2 (40-bit), PTFSv8_4K (bit 12). */
    m->regs[0x20 / 4] = (1u << 30) | 16u;
    m->regs[0x24 / 4] = (3u << 28) | 8u;
    m->regs[0x28 / 4] = (1u << 12) | 0x222u;
    /* Preserve existing UFS stream 0x60 on bank 1 and touch stream 0xa3 on bypass bank 7. */
    m->regs[0x800 / 4] = 0x80000060u;
    m->regs[0xc00 / 4] = 1u;
    m->regs[(0x1000 + 4) / 4] = 0x1f300u;
    m->regs[(0x1800 + 4) / 4] = 1u;
    m->regs[0x804 / 4] = 0x800000a3u;
    m->regs[0xc04 / 4] = 7u;
    m->regs[(0x1000 + 7 * 4) / 4] = 0x10000u;
    m->regs[(0x1800 + 7 * 4) / 4] = 1u;
}

int main(void) {
    struct model *m = calloc(1, sizeof(*m));
    assert(m);

    /* 1. SM7675 UART10 RCG rates, DFS handoff, invalid baud and bounded timeout. */
    struct sm7675_geni_clock_io clock = {m, rd_clk, wr_clk, delay_us};
    uint32_t hz = 0;
    const uint32_t rates[] = {115200, 3000000, 3200000, 4000000};
    const uint32_t freqs[] = {7372800, 48000000, 51200000, 64000000};
    const uint32_t root = SM7675_GCC_QUP1_S2_CMD_RCGR;
    for (unsigned i = 0; i < 4; i++) {
        memset(m, 0, sizeof(*m));
        m->regs[(root + 0x14) / 4] = 1u;
        m->regs[(root + 4) / 4] = 0x80000000u;
        assert(!sm7675_geni_uart10_source(&clock, rates[i], &hz) && hz == freqs[i]);
        assert(m->regs[(root + 0x14) / 4] == 0u);
        assert(m->regs[(root + 4) / 4] == 0x80002601u);
        uint32_t m_val = m->regs[(root + 8) / 4] & 0xffffu;
        uint32_t n_val = m_val + (~m->regs[(root + 12) / 4] & 0xffffu);
        assert(300000000ULL * m_val / n_val == freqs[i]);
        assert((~m->regs[(root + 16) / 4] & 0xffffu) == n_val);
    }
    unsigned before = m->writes;
    assert(sm7675_geni_uart10_source(&clock, 123, &hz) == -EINVAL && m->writes == before);
    memset(m, 0, sizeof(*m));
    m->regs[(root + 0x14) / 4] = 1u;
    m->dfs_stuck = true;
    assert(sm7675_geni_uart10_source(&clock, 3000000, &hz) == -EIO);
    memset(m, 0, sizeof(*m));
    m->clock_stuck = true;
    assert(sm7675_geni_uart10_source(&clock, 3000000, &hz) == -ETIMEDOUT && m->delays == 1000);

    /* 2. Apps-SMMU bypass mapping for SM7675 QUP1 SE2 (SID 0x423) sharing bank 7 with touch (0xa3). */
    init_smmu(m);
    struct smmu_io smmu = {m, rd_smmu, wr_smmu};
    struct smmu_plan plan = smmu_plan_bypass(&smmu, SM7675_QUP1_SID, 0x8c0100000ULL, 32768);
    assert(!plan.error && plan.slot == 2 && plan.bank == 7 && !plan.reuse && plan.bank_ready);
    assert(!smmu_configure_bypass(&smmu, &plan, SM7675_QUP1_SID));
    assert(m->regs[0x808 / 4] == (0x80000000u | SM7675_QUP1_SID));
    assert(m->regs[0xc08 / 4] == 7u);
    assert(m->regs[0x800 / 4] == 0x80000060u && m->regs[0xc00 / 4] == 1u);
    assert(m->regs[0x804 / 4] == 0x800000a3u && m->regs[0xc04 / 4] == 7u);
    /* Second plan reuses existing stream mapping cleanly. */
    struct smmu_plan reuse = smmu_plan_bypass(&smmu, SM7675_QUP1_SID, 0x8c0100000ULL, 32768);
    assert(!reuse.error && reuse.reuse && reuse.slot == 2 && reuse.bank == 7);
    assert(!smmu_configure_bypass(&smmu, &reuse, SM7675_QUP1_SID));
    /* Conflicting mapping on another bank is rejected. */
    m->regs[0xc08 / 4] = 1u;
    struct smmu_plan bad = smmu_plan_bypass(&smmu, SM7675_QUP1_SID, 0x8c0100000ULL, 32768);
    assert(bad.error != NULL);

    /* 3. WCN6750 (Moselle) firmware filename validator. */
    assert(sheng_bt_firmware_name_valid("qca/msbtfw12.mbn"));
    assert(sheng_bt_firmware_name_valid("qca/msbtfw12.tlv"));
    assert(sheng_bt_firmware_name_valid("qca/msnv12.bin"));
    assert(sheng_bt_firmware_name_valid("qca/msnv12.b01"));
    assert(sheng_bt_firmware_name_valid("qca/msnv12.b0102"));
    assert(!sheng_bt_firmware_name_valid(NULL));
    assert(!sheng_bt_firmware_name_valid(""));
    assert(!sheng_bt_firmware_name_valid("qca/hmtbtfw20.tlv"));
    assert(!sheng_bt_firmware_name_valid("qca/hmtnv20.bin"));
    assert(!sheng_bt_firmware_name_valid("../msbtfw12.mbn"));
    assert(!sheng_bt_firmware_name_valid("qca/MSBTFW12.mbn"));
    assert(!sheng_bt_firmware_name_valid("qca/msnv12.b1"));
    assert(!sheng_bt_firmware_name_valid("qca/msnv12.b12345"));
    assert(!sheng_bt_firmware_name_valid("qca/msbtfw12.bin"));

    free(m);
    puts("PASS: SM7675 UART10 RCG rates/DFS/timeout, apps-SMMU SID 0x423 bypass sharing and WCN6750 firmware filter");
    return 0;
}
