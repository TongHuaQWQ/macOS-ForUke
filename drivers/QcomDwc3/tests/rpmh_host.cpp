// Host test of QcomRpmh.h: the TCS register sequence, against the order rpmh-rsc.c requires.
//
// Build: clang -c ../../../package/QcomRpmh.c -o /tmp/QcomRpmh.o &&
//        clang++ -std=c++17 -I../../../package rpmh_host.cpp /tmp/QcomRpmh.o -o /tmp/rpmh_host && /tmp/rpmh_host
//
// The sequence is the whole point of this file. The 2026-11-21 usb6 boot of this board stopped on the fifth rail vote with nothing
// printed, because the write list cleared the AMC mode bits with a single store to zero. rpmh-rsc.c's __tcs_set_trigger() says why
// that is not allowed -- "While clearing ensure that the AMC mode trigger is cleared and then the mode enable is cleared" -- and the
// RSC answers an illegal mode sequence by not answering at all, which takes the whole kernel with it. This test pins the order, the
// addresses and the masks so that a later edit cannot quietly reintroduce the single store.
#include "QcomRpmh.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {
constexpr uint32_t kDrvId = 0x00030200;   // this board's DRV-2 id register: RSC version 3.2
constexpr unsigned kTcs = 2;              // third (and last) ACTIVE TCS of live.dts:6336
constexpr unsigned kTcsOffset = 0xd00;    // qcom,tcs-offset of that drv@2 node
constexpr uint32_t kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24;

struct Write {uint32_t offset, value, mask;};

std::vector<Write> sequence(unsigned count, int wait) {
    QcomRpmhWrite writes[24];
    const QcomRscLayout layout = qcom_rsc_layout(kDrvId);
    const uint32_t addresses[4] = {0x0004a3e8, 0x0004a3ec, 0x0004a3f0, 0x0004a3f4}, values[4] = {7, 880, 1, 9};
    const unsigned n = qcom_rpmh_active_request(&layout, kTcs, addresses, values, count, wait, writes, 24);
    assert(n);
    std::vector<Write> out;
    for (unsigned i = 0; i < n; ++i) out.push_back({writes[i].offset, writes[i].value, writes[i].mask});
    return out;
}

// Applies one write the way the driver's apply() does, and returns the register file.
struct Regs {
    std::vector<uint32_t> words = std::vector<uint32_t>(0x10000 / 4, 0);
    void apply(const Write &w) {
        uint32_t &slot = words[(kTcsOffset + w.offset) / 4];
        slot = w.mask == QCOM_RPMH_WHOLE_WORD ? w.value : (slot & ~w.mask) | (w.value & w.mask);
    }
    uint32_t at(unsigned offset) const {return words[(kTcsOffset + offset) / 4];}
};

void testVersionSelectsTheTables() {
    const QcomRscLayout three = qcom_rsc_layout(0x00030200), two = qcom_rsc_layout(0x00020200);
    // rpmh_rsc_reg_offsets_ver_3_0: the TCS registers start at 0x20, the command stride is 24 and CMD_STATUS is 0x40.
    assert(three.tcs_stride == 672 && three.cmd_stride == 24);
    assert(three.irq_enable == 0x00 && three.irq_status == 0x04 && three.irq_clear == 0x08);
    assert(three.wait_for_cmpl == 0x20 && three.control == 0x24 && three.status == 0x28 && three.cmd_enable == 0x2c);
    assert(three.cmd_msgid == 0x34 && three.cmd_addr == 0x38 && three.cmd_data == 0x3c && three.cmd_status == 0x40);
    // rpmh_rsc_reg_offsets_ver_2_7, which is what a version-2 id has to get.
    assert(two.cmd_stride == 20 && two.control == 0x14 && two.cmd_enable == 0x1c && two.cmd_msgid == 0x30 && two.cmd_status == 0x3c);
}

void testRequestOrder() {
    const std::vector<Write> w = sequence(1, 1);
    const QcomRscLayout l = qcom_rsc_layout(kDrvId);
    const uint32_t base = l.tcs_stride * kTcs;
    // rpmh_rsc_send_data() clears the previous transfer's masks before anything else.
    assert(w[0].offset == base + l.cmd_enable && w[0].value == 0 && w[0].mask == QCOM_RPMH_WHOLE_WORD);
    assert(w[1].offset == base + l.wait_for_cmpl && w[1].value == 0);
    // The command: 4-byte write, response requested (the owner waits on CMD_STATUS), at the layout's command stride.
    assert(w[2].offset == base + l.cmd_msgid && w[2].value == (8u | 1u << 8 | 1u << 16));
    assert(w[3].offset == base + l.cmd_addr && w[3].value == 0x0004a3e8);
    assert(w[4].offset == base + l.cmd_data && w[4].value == 7);
    // The completion mask, then the command it applies to.
    assert(w[5].offset == base + l.wait_for_cmpl && w[5].value == 1 && w[5].mask == QCOM_RPMH_WHOLE_WORD);
    assert(w[6].offset == base + l.cmd_enable && w[6].value == 1);
    // __tcs_set_trigger(true): trigger off, enable off, enable on, trigger on -- each clear touching one bit.
    assert(w[7].offset == base + l.control && w[7].value == 0 && w[7].mask == kAmcTrigger);
    assert(w[8].offset == base + l.control && w[8].value == 0 && w[8].mask == kAmcEnable);
    assert(w[9].offset == base + l.control && w[9].value == kAmcEnable && w[9].mask == QCOM_RPMH_WHOLE_WORD);
    assert(w[10].offset == base + l.control && w[10].value == (kAmcEnable | kAmcTrigger) && w[10].mask == QCOM_RPMH_WHOLE_WORD);
    assert(w.size() == 11);
    // A request with no response is mainline's RPMH_ACTIVE_ONLY_STATE batch: no RESP_REQ and no completion mask.
    const std::vector<Write> async = sequence(1, 0);
    assert(async[2].value == (8u | 1u << 16));
    assert(async[5].value == 0);
}

// The regression: a TCS left with both AMC mode bits set -- what every completed transfer leaves behind -- has to be re-armed
// without a store that drops both at once, and the trigger must not come up before the enable.
void testAmcModeOrder() {
    Regs r;
    r.words[(kTcsOffset + qcom_rsc_layout(kDrvId).tcs_stride * kTcs + qcom_rsc_layout(kDrvId).control) / 4] = kAmcEnable | kAmcTrigger;
    const QcomRscLayout l = qcom_rsc_layout(kDrvId);
    const unsigned control = l.tcs_stride * kTcs + l.control;
    for (const Write &w : sequence(1, 1)) {
        const uint32_t before = r.at(l.tcs_stride * kTcs + l.control);
        r.apply(w);
        if (w.offset != control) continue;
        const uint32_t after = r.at(l.tcs_stride * kTcs + l.control);
        // The enable may only fall once the trigger is already down, and never in the same store as it.
        if ((before & kAmcEnable) && !(after & kAmcEnable)) assert(!(before & kAmcTrigger));
        // A trigger without an enable is a TCS the RSC will not run.
        if (after & kAmcTrigger) assert(after & kAmcEnable);
    }
    // The TCS ends armed and triggered, which is what the RSC needs to start the transfer.
    assert(r.at(l.tcs_stride * kTcs + l.control) == (kAmcEnable | kAmcTrigger));
    assert(r.at(l.tcs_stride * kTcs + l.cmd_enable) == 1);
}

// tcs_tx_done(): the masks go, the interrupt bit is acknowledged, and the control register is left alone -- the next request clears
// the AMC bits itself, in the only order that works.
void testFinish() {
    const QcomRscLayout l = qcom_rsc_layout(kDrvId);
    const uint32_t base = l.tcs_stride * kTcs;
    QcomRpmhWrite w[8];
    const unsigned n = qcom_rpmh_finish(&l, kTcs, w, 8);
    assert(n == 3);
    assert(w[0].offset == base + l.cmd_enable && w[0].value == 0);
    assert(w[1].offset == base + l.wait_for_cmpl && w[1].value == 0);
    assert(w[2].offset == l.irq_clear && w[2].value == (1u << kTcs));
    for (unsigned i = 0; i < n; ++i) assert(w[i].offset != base + l.control);
}

// qcom_rsc_layout() is only right if the id register is read at the DRV base; the board's other USB path asserts the same value.
void testIdRegister() {
    assert(((kDrvId >> 16) & 0xff) == 3);
    assert(sizeof(QcomRscLayout) == 13 * sizeof(uint32_t));   // 672-byte TCS, 13 registers of the version 3 table
}

// A cmd-db header with one slave, one resource and two ARC levels, laid out as qcom_cmd_db_find() documents.
void testCmdDb() {
    uint8_t db[512] = {0};
    auto put32 = [&](size_t at, uint32_t v) {memcpy(db + at, &v, 4);};
    auto put16 = [&](size_t at, uint16_t v) {memcpy(db + at, &v, 2);};
    put32(4, QCOM_CMD_DB_MAGIC);
    put16(8, 1);       // slave 0 present
    put16(10, 0x00);   // its header records start at 144
    put16(12, 0x40);   // its data area starts at 144 + 0x40
    put16(14, 1);      // one entry
    const size_t entry = 144;
    memcpy(db + entry, "ldoe1\0\0\0", 8);
    put32(entry + 16, 0x0004a3e8);
    put16(entry + 20, 4);   // two 16-bit levels
    put16(entry + 22, 0);   // at data offset 0
    put16(144 + 0x40, 0);   // level 0 turns the vote off
    put16(144 + 0x42, 7);   // level 1 is the one an 880 mV request takes
    QcomRpmhResource r;
    memset(&r, 0, sizeof r);
    assert(qcom_cmd_db_find(db, sizeof db, "ldoe1", &r) == QcomRpmhOk);
    assert(r.address == 0x0004a3e8 && r.level_count == 2 && r.levels[1] == 7);
    assert(qcom_cmd_db_find(db, sizeof db, "ldoe3", &r) == QcomRpmhMissing);
    uint32_t index = 99;
    assert(qcom_rpmh_level_index(&r, 880, &index) == QcomRpmhOk);
    // A vreg address is 32-bit little endian in the entry, and a truncated database is corruption, not a miss.
    assert(qcom_cmd_db_find(db, 200, "ldoe1", &r) == QcomRpmhCorrupt);
    db[4] = 0;
    assert(qcom_cmd_db_find(db, sizeof db, "ldoe1", &r) == QcomRpmhNoDb);
}
}  // namespace

int main() {
    testVersionSelectsTheTables();
    testRequestOrder();
    testAmcModeOrder();
    testFinish();
    testIdRegister();
    testCmdDb();
    printf("rpmh_host: all checks passed\n");
    return 0;
}
