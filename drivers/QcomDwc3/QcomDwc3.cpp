// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8550 USB device controller behind Apple's own stack. The DWC3 programming model, endpoints, control transfers and everything
// above them (IOUSBDeviceFamily, AppleUSBDeviceMux, configd's USBDeviceConfiguration, lockdownd) stay the unchanged system parts:
// this class only supplies what an Apple SoC subclass of AppleUSBXDCI supplies, namely the register window, the interrupt, power
// and the PHY. Hardware facts come from mainline Linux: sm8550.dtsi, gcc-sm8550.c, dwc3-qcom.c, phy-snps-eusb2.c.
#include "OriginalUsbXdciABI.h"
#include "OriginalUsbXhciABI.h"
#include "QcomEusb2Phy.h"
#include "QcomQmpUsb3.h"
#include "GeniI2c.h"
#include "QcomUcsi.h"
#include "QcomRpmh.h"
#include "AppsSmmuLock.h"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
// uke: this kernel's architected timer never delivers an interrupt, and IOSleep() waits on that timer, so
// a sleep here is a permanent wait (b251-usbdwc3 hung inside the first IOSleep the driver takes). Every
// delay becomes a spin on the timebase, which is what IODelay() already is; the vendored call sites stay
// verbatim. See package/QcomDwc3Platform.c for uke_delay_ms().
extern "C" void uke_delay_ms(uint32_t);
#define IOSleep(ms) uke_delay_ms((uint32_t)(ms))
extern "C" {
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
int armcpu_spi_mask(uint32_t, bool);
bool sm8550_gcc_update32(volatile uint32_t *, uint32_t, uint32_t);
// [0] stage (negative: the step that failed), [1] GSNPSID, [2] GUID-free hardware parameter 0, [3] parameter 3, [4] parameter 7,
// [5] GDSC USB30, [6] GDSC PHY, [7] SMMU slot | bank << 16, [8] repeater status | poll tries << 8 | SPMI APID << 16,
// [9] RPMh rails found, [10] VBUS override state,
// [11] interrupts, [12] powerOn calls, [13] powerOff calls, [14] controllerReset result, [15] DSTS at power on, [16] DCFG after
// reset, [17] GCTL after reset, [18] GUSB2PHYCFG after reset, [19] start result, [20] power state request result, [21] kext load result,
// with usb=2, every 2 s: [22] GSBUSCFG0, [23] GSBUSCFG1, [24] GUCTL1, [25] SMMU global fault, [26] DSTS, [27] DALEPENA,
// [28] GEVNTCOUNT, [29] DEPCMD of EP0 OUT, [30] bypass bank fault status, [31] GEVNTSIZ.
// [32] retimer: chip id | stage << 16, [33] QMP PCS status, [34] orientation GPIO, [35] VBUS changes | last value << 16,
// [44] role applied | wanted << 8 | core << 16, [45] role changes, [46] xHCI load << 16 | start, [47] USBCMD, [48] USBSTS, [49] [50] PORTSC 1 and 2,
// [51] HCSPARAMS1,
// [40] UCSI state | version << 16, [41] UCSI connector status, [42] CCI, [43] changes | errors << 16,
// [36] bit mask of the SuperSpeed link states seen (GDBGLTSSM bits 25:22), [37] GDBGLTSSM, [38] GUSB3PIPECTL, [39] cable test steps.
volatile uint32_t native_usb_stats[54];
}
namespace {
// The DWC3 global, device and OTG registers in the window Apple's class addresses: base + 0xc000.
// uke / SM7675 "cliffs". Every address below is the board's own, not sheng's SM8550: the DWC3 wrapper,
// the Apple register window, QSCRATCH, GCC, the eUSB2 PHY and the apps SMMU happen to be identical, but
// TCSR (0x1f40000 is this board's tcsrcc, live.dts:13408; the tcsr-mutex syscon is the same base with a
// different node identity, live.dts:14342), the apps RSC (rsc@17a00000, live.dts:6320) and the USB
// interrupt are not.
constexpr uint64_t kCore = 0x0a600000, kAppleWindow = kCore + 0xc000, kQscratch = kCore + 0xf8800, kGcc = 0x100000,
    // The TCSR clock controller's base (tcsrcc@1f40000, live.dts:13408). Note that its USB2 clkref gate is NOT at
    // an offset inside the first page: it is at +0xb1118, which is clkrefGate()'s business (0x01ff1118).
    kTcsr = 0x1f40000, kPhy = 0x088e3000, kSmmu = 0x15000000, kCmdDb = 0x81c60000;
// TLMM is pinctrl@f000000, reg = <0xf000000 0x1000000> (live.dts:7701; the same node in the vendor DT). The
// 0xf100000 this driver inherited sits one MiB into that aperture, which is not "the same block shifted": each pin
// has its own 0x1000 page, so 0xf100000 + n*0x1000 is GPIO 256+n. Pins past this SoC's last one are not implemented,
// and reading one stopped the bus on the 2026-11-21 usb9 boot (the panel's last line was the mark before it).
constexpr uint64_t kTlmm = 0xf000000;
// Two things this board does not have, and sheng's does. This board's DT declares usb_qmp_dp_phy (ssphy@88e8000) and
// no redriver/retimer node at all -- grep ps5169/retimer/redriver in live.dts and vendor_boot.dts finds nothing --
// and usb0 carries no orientation-gpios property. So the PS5169 bring-up (GPIO 7 as its reset, GPIO 28/29 as its
// bus, and the qupv3 clocks behind it) is another board's wiring, and the orientation pin is unknown here. Both are
// left out rather than pointed at pins that may belong to a panel or a touch controller.
constexpr bool kBoardHasRetimer = false, kBoardHasOrientationGpio = false;
// The apps RSC is three 64 KiB drive windows, not one block (live DT, rsc@17a00000):
//   reg = <0x17a00000 0x10000  0x17a10000 0x10000  0x17a20000 0x10000>
//   reg-names = "drv-0", "drv-1", "drv-2", and the AP talks through drv-2
//   (apps_rsc_drv2 = "/soc/rsc@17a00000/drv@2").
// The 2026-11-21 usb5 boot read drv-0 instead and the very first word of that unpowered
// window aborted: far 0xfffffff5d42800 equalled the window base right after stage 22.
constexpr uint64_t kRsc = 0x17a20000;
// qcom,tcs-offset of that DRV (live.dts:6332): the TCS blocks start 0xd00 bytes into the window, and rpmh-rsc.c adds the DRV's
// tcs-offset to the base before any register offset of its tables.
constexpr uint32_t kRscTcsBase = 0xd00;
// kIrq is the GIC INTID: Linux SPI 265 (work/re/data/ufs-cliffs.json host.irq.intid is 297 for UFS, and
// this board's USB node is interrupts = <0 265 4>), i.e. 265 + 32.
constexpr uint32_t kCmdDbSize = 0x20000, kIrq = 265 + 32, kSid = 0x40;
// How much of the cmd-db window this board answers on. The 2026-11-21 boot faulted at
// far=0xfffffff16d58c000 — page offset 0xc000, i.e. 48 KiB in — with the platform error
// counter still reading #1, so everything below 0xc000 is readable and the window ends
// there. Read that far and no further; the header length can only lower it.
constexpr uint32_t kCmdDbReadable = 0xc000;
// The TCS this driver votes in, inside the ACTIVE group of this board's DT (live.dts:6336):
//   qcom,tcs-config = <ACTIVE 3>, <SLEEP 2>, <WAKE 2>, <CONTROL 0>, <FAST_PATH 1>
// rpmh-rsc.c numbers the groups in that order, so ACTIVE is TCS 0, 1 and 2 and the last one is ours --
// out of the way of the TCS the bootloader's last vote may still hold.
constexpr unsigned kOwnedTcs = 2;
// qcom,drv-id of the node this driver talks through (live.dts:6331, drv@2): DRV_PRNT_CHLD_CONFIG gives each DRV id its own field.
constexpr unsigned kDrvIdForPanel = 2;
struct Region {IOMemoryMap *map; volatile uint32_t *va;};
Region gGcc, gScratch, gPhy, gTcsrMap;
bool gSuperSpeed;
// The orientation pin's last sample, read once when the board declares one (kBoardHasOrientationGpio).
uint32_t gTlmmOrientation;
// Every stage is logged as it is entered, not only reported once bringUp() returns: a round that hangs in
// a later stage has to say which ones were reached, and the panel is the only channel on this board.
void stage(int value) {native_usb_stats[0] = (uint32_t)value; IOLog("qcom-dwc3: stage %d\n", value);}
// A breadcrumb inside a stage. stage() says which step of bringUp() a boot reached; these say which register group inside that
// step, because the steps below touch blocks whose clock may not be running yet and a store to an unclocked block on this SoC does
// not fault -- it never completes, and the whole kernel goes with it (that is how the 2026-11-21 usb6/7 boots ended). One line
// each, so the tail of the panel names the group the boot died in.
void mark(const char *what) {IOLog("qcom-dwc3: mark %s\n", what);}
// Mapped through ml_io_map(), not through IODeviceMemory::map().
//
// The 2026-11-21 boot (image usb2) shows why: the worker starts, prints its first line and
// "wait 250 ms at stage 0", and then never reaches stage 1, while the panel is full of
// "Fileset-wait: IOResources timeout 18446744073709551615 (Infinite)" and
// "Couldn't alloc class NativeVFS". This probe runs from the first statement of
// bsd_autoconf(), before IOKit's boot has published IOResources, and
// IODeviceMemory::map() waits on exactly those services. ml_io_map() is the pre-IOKit path
// (the same one QcomXnuUsbDebug uses for its own windows) and never waits on a service.
extern "C" vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);
// PE_parse_boot_argn() is already declared by the headers this unit includes -- forceHostThread() below has parsed `usb-back` with
// it since the role work, so `usb-rails` needs no new import.

bool mapRegion(Region &r, uint64_t pa, uint64_t bytes) {
    r.map = nullptr;
    const vm_offset_t va = ml_io_map(static_cast<vm_offset_t>(pa), static_cast<vm_size_t>(bytes));
    r.va = va ? reinterpret_cast<volatile uint32_t *>(va) : nullptr;
    return r.va != nullptr;
}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
bool waitBits(volatile uint32_t *reg, uint32_t mask, uint32_t value, unsigned ms) {
    for (unsigned i = 0; i < ms * 10; ++i) {
        if ((*reg & mask) == value) return true;
        IODelay(100);
    }
    return (*reg & mask) == value;
}
// Rails of the eUSB2 PHY, its repeater and the QMP PHY, the reference clock and the path to memory (dwc3-qcom usb-ddr).
bool powerRails() {
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!db) return false;
    Region dbw = {}, rsc = {};
    bool ok = mapRegion(dbw, kCmdDb, kCmdDbSize) && mapRegion(rsc, kRsc, 0x10000);
    // Both mappings and the sweep length are printed before the sweep, because the sweep is where the
    // 2026-11-21 boots stop: 'stage 2' then 'platform error #1 ... (ignored)' and nothing after, with no
    // chance for a line placed past the fault to appear. stage 21/22/23 mark the sub-steps so the next
    // photograph says which one was reached even if the console's tail is overwritten.
    IOLog("qcom-dwc3: cmd-db map=%p rsc map=%p size=%u readable=%u rsc_pa=%llx\n", dbw.va, rsc.va,
          (unsigned)kCmdDbSize, (unsigned)kCmdDbReadable, (unsigned long long)kRsc);
    if (ok) {
        stage(21);
        /* Bounded by the header's own length, not by kCmdDbSize. The 2026-11-21 boot shows why:
         * the sweep below aborted at 'qcom-dwc3: stage 2' with
         *   platform error #1 far=0xfffffff16d58c000 pc=0xfffffff00bbea9ac (ignored)
         * i.e. 48 KiB into the 128 KiB window — this board's cmd-db block does not have 128 KiB of
         * readable space, and everything past its real end is an unpowered slave that answers with
         * an asynchronous abort. Linux reads cmd_db_header.len for the same reason
         * (drivers/soc/qcom/cmd-db.c). Word 3 is that length in the downstream header; when it is
         * not a plausible value, clamp to the first 64 KiB instead of 128 KiB. */
        uint32_t length = dbw.va[3];
        if (length < 16 || length > kCmdDbSize || (length & 3)) { length = kCmdDbReadable; }
        if (length > kCmdDbReadable) { length = kCmdDbReadable; }
        for (uint32_t i = 0; i < length; i += 4) {const uint32_t w = dbw.va[i / 4]; memcpy(db + i, &w, 4);}
        for (uint32_t i = length; i < kCmdDbSize; i += 4) {const uint32_t zero = 0; memcpy(db + i, &zero, 4);}
        stage(22);
        const uint32_t rscWord = rsc.va[0];
        stage(23);
        const QcomRscLayout layout = qcom_rsc_layout(rscWord);
        // One line, before any vote: the previous boot ended in stage 2 with no stage 3 and no
        // verdict line, so the next wall has to be visible from the panel.
        IOLog("qcom-dwc3: cmd-db len=%u rsc=%08x tcs_stride=%u\n", (unsigned)length,
              (unsigned)rscWord, (unsigned)layout.tcs_stride);
        AppsSmmuLock guard;
        // The AP's TCS block, addressed the way rpmh-rsc.c addresses it: DRV base + qcom,tcs-offset (0xd00 in this board's DT,
        // live.dts:6332), then one block per TCS, each block's registers at the layout's offsets inside it.
        const uint32_t tcsBase = kRscTcsBase + layout.tcs_stride * kOwnedTcs;
        // rpmh_rsc_probe() leaves the interrupts of the active TCSes enabled, and IRQ_STATUS only latches for an enabled one.
        rsc.va[(kRscTcsBase + layout.irq_enable) / 4] |= 1u << kOwnedTcs;
        // What the DRV says about itself, before anything is written: DRV_PRNT_CHLD_CONFIG (drv base + 0x0c) carries the TCS count of
        // each DRV id in bits 6*id+5:6*id and the commands per TCS in 31:27 (rpmh-rsc.c, DRV_NUM_TCS_/DRV_NCPT_), and bit 24 is the
        // hardware solver. A TCS count that does not reach ours, or zero commands per TCS, would explain a trigger that never
        // completes -- and neither can be read from the panel any other way.
        const uint32_t config = rsc.va[0x0c / 4];
        IOLog("qcom-dwc3: rsc drv=%u prnt=%08x tcs=%u ncpt=%u solver=%u\n", (unsigned)kDrvIdForPanel, (unsigned)config,
              (unsigned)((config >> (6 * kDrvIdForPanel)) & 0x3f), (unsigned)((config >> 27) & 0x1f), (unsigned)((config >> 24) & 1));
        // State of the TCS this boot inherits: RSC_DRV_CONTROL still carries the bootloader's AMC mode bits, RSC_DRV_STATUS is zero
        // while the TCS is busy, and the interrupt registers say whether anything is in flight. Printed once, before any vote.
        IOLog("qcom-dwc3: rsc tcs=%u ctl=%08x sts=%08x irqen=%08x irqsts=%08x\n", (unsigned)kOwnedTcs,
              (unsigned)rsc.va[(tcsBase + layout.control) / 4], (unsigned)rsc.va[(tcsBase + layout.status) / 4],
              (unsigned)rsc.va[(kRscTcsBase + layout.irq_enable) / 4], (unsigned)rsc.va[(kRscTcsBase + layout.irq_status) / 4]);
        // One register write of the sequence. A masked write is the read-modify-write rpmh-rsc.c's write_tcs_reg() uses to touch the
        // AMC mode bits one at a time, and every write is read back before the next one, which is what its write_tcs_reg_sync() buys.
        auto apply = [&](const QcomRpmhWrite &w) {
            volatile uint32_t *slot = rsc.va + (kRscTcsBase + w.offset) / 4;
            *slot = w.mask == QCOM_RPMH_WHOLE_WORD ? w.value : (*slot & ~w.mask) | (w.value & w.mask);
            (void)*slot;
            barrier();
        };
        // One active-only vote. rpmh_rsc_send_data() claims a free TCS through find_free_tcs(), which polls RSC_DRV_STATUS until it
        // reads non-zero, and claim_tcs_for_req() refuses a second request to an address that is still in flight -- "the h/w does
        // not like if we send a request to the same address, when one is already in-flight or being processed". Three votes per rail
        // go to the same VREG resource (offset, +4, +8), so each one has to be finished before the next: that is what the STATUS
        // poll below buys, and why this driver reuses one TCS instead of running them past each other. The command is then waited
        // for through RSC_DRV_CMD_STATUS -- bits 8 and 16 are the issue and the completion of this TCS's command (the pair this
        // board's other USB path already decodes). Both waits are bounded on the timebase: nothing here sleeps on the timer that
        // never fires. A one-line record goes out per vote, because a vote that stalls the RSC takes the whole kernel with it and
        // the panel is the only channel -- the last line then says which write it was.
        unsigned votes = 0, completed = 0;
        auto send = [&](uint32_t address, uint32_t value) {
            const unsigned index = ++votes;
            IOLog("qcom-dwc3: vote %u %08x=%u\n", index, (unsigned)address, (unsigned)value);
            volatile uint32_t *status = rsc.va + (tcsBase + layout.status) / 4;
            for (unsigned i = 0; i < 200 && !*status; ++i) IODelay(10);
            if (!*status) {IOLog("qcom-dwc3: vote %u tcs busy\n", index); return false;}
            QcomRpmhWrite writes[16];
            const unsigned n = qcom_rpmh_active_request(&layout, kOwnedTcs, &address, &value, 1, 1, writes, sizeof writes / sizeof writes[0]);
            if (!n) {IOLog("qcom-dwc3: vote %u not described\n", index); return false;}
            for (unsigned i = 0; i < n; ++i) apply(writes[i]);
            volatile uint32_t *cmd = rsc.va + (tcsBase + layout.cmd_status) / 4;
            constexpr uint32_t kIssuedAndComplete = (1u << 8) | (1u << 16);
            unsigned polls = 0;
            for (; polls < 500; ++polls) {
                if ((*cmd & kIssuedAndComplete) == kIssuedAndComplete) break;
                IODelay(10);
            }
            if (polls == 500) {
                IOLog("qcom-dwc3: vote %u stuck cmd=%08x sts=%08x irq=%08x ctl=%08x\n", index, (unsigned)*cmd, (unsigned)*status,
                      (unsigned)rsc.va[(kRscTcsBase + layout.irq_status) / 4], (unsigned)rsc.va[(tcsBase + layout.control) / 4]);
                return false;
            }
            const unsigned m = qcom_rpmh_finish(&layout, kOwnedTcs, writes, sizeof writes / sizeof writes[0]);
            for (unsigned i = 0; i < m; ++i) apply(writes[i]);
            ++completed;
            return true;
        };
        static const struct {const char *name; uint32_t mv;} rails[] = {
            {"ldoe1", 880}, {"ldoe3", 1200}, {"ldob15", 1800}, {"ldob5", 3104}, {"ldof3", 912}};
        for (unsigned i = 0; i < sizeof rails / sizeof rails[0] && ok; ++i) {
            QcomRpmhResource res;
            if (qcom_cmd_db_find(db, kCmdDbSize, rails[i].name, &res)) {
                IOLog("qcom-dwc3: rail %s not in cmd-db\n", rails[i].name);
                ok = false;
                break;
            }
            if (!send(res.address + 8, 7) || !send(res.address, rails[i].mv) || !send(res.address + 4, 1)) {
                IOLog("qcom-dwc3: rail %s not voted\n", rails[i].name);
                ok = false;
                break;
            }
            native_usb_stats[9] = native_usb_stats[9] | (1u << i);
        }
        QcomRpmhResource res;
        if (ok && !qcom_cmd_db_find(db, kCmdDbSize, "xo.lvl", &res)) {if (!send(res.address, 3)) ok = false;} else ok = false;
        // SN2 carries qnm_aggre1_noc: vote = ceil(ceil(kBps * width / buswidth) * 1000 / unit), buswidth 16, 2500 MB/s peak.
        if (ok && !qcom_cmd_db_find(db, kCmdDbSize, "SN2", &res)) {
            uint32_t vote = 0x400;
            if (res.level_count >= 3 && (res.levels[0] | (uint32_t)res.levels[1] << 16) && res.levels[2]) {
                const uint64_t unit = res.levels[0] | (uint32_t)res.levels[1] << 16;
                const uint64_t x = (((2500000ull * res.levels[2] + 15) / 16) * 1000 + unit - 1) / unit;
                vote = x < 1 ? 1 : x > 0x3fff ? 0x3fff : (uint32_t)x;
            }
            if (send(res.address, 1u << 30 | 1u << 29 | vote << 14 | vote)) native_usb_stats[9] = native_usb_stats[9] | 0x80000000u;
            else ok = false;
        } else ok = false;
        IOLog("qcom-dwc3: rails votes=%u completed=%u\n", votes, completed);
    }
    (void)dbw; (void)rsc;   // ml_io_map windows live for the boot

    IOFreeData(db, kCmdDbSize);
    return ok;
}
bool gdsc(uint32_t offset, unsigned statIndex) {
    volatile uint32_t *reg = gGcc.va + offset / 4;
    if (!sm8550_gcc_update32(reg, 1u, 1u << 11)) return false;   // SW_COLLAPSE off, RETAIN_FF_ENABLE on
    const bool ok = waitBits(reg, 0x80000000u, 0x80000000u, 100);
    native_usb_stats[statIndex] = *reg;
    return ok;
}
bool branch(uint32_t offset) {
    volatile uint32_t *reg = gGcc.va + offset / 4;
    if (!sm8550_gcc_update32(reg, 0, 1u)) return false;
    return waitBits(reg, 0x80000000u, 0, 100);
}
bool rcg(uint32_t cmd, uint32_t cfg) {
    volatile uint32_t *c = gGcc.va + cmd / 4;
    if ((c[1] & 0x71fu) == cfg) return true;
    c[1] = (c[1] & ~0x71fu) | cfg;
    barrier();
    c[0] = c[0] | 1u;
    barrier();
    return waitBits(c, 1u, 0, 10);
}
// The USB2 PHY's reference clock gate lives in the TCSR clock controller (tcsrcc@1f40000, compatible
// "qcom,pineapple-tcsrcc", live.dts:13408): a clk_branch2 register, bit 0 enables it. Both of this board's own
// implementations already do exactly this and nothing else --
//   package/UkeUsbBoard.cpp:64   refClock(): tcsrcc 0x01f40000 + 0xb1118, static_assert(pa == 0x01ff1118)
//   package/qcom-usb-debug/kernel/QcomUsbBoardInit.c:118  the same pair, in C
// and docs/USB2-20261115.md:42 read CBCR=0x00000001 back out of that address under TWRP, so it is measured on
// this unit. The two writes this driver inherited instead -- 0x1fd5108 and 0x1fd5118, sheng's SM8550 TCSR --
// were rebased onto this board's base and so aimed at 0x1f40118, inside the tcsr_mutex aperture
// (live.dts:14342, syscon@1f40000 + 0x20000). That block does not implement that address, the store never
// completes, and the kernel goes with it: the usb8 panel's last line is the mark immediately before it.
bool clkrefGate(uint64_t pa) {
    Region r = {};
    if (!mapRegion(r, pa & ~0xfffull, 0x1000)) return false;
    volatile uint32_t *reg = r.va + (pa & 0xfffu) / 4;
    const uint32_t before = *reg;
    if (!(before & 1u)) {*reg = before | 1u; barrier(); IODelay(20);}
    const uint32_t after = *reg;
    (void)r;
    IOLog("qcom-dwc3: mark tcsrcc clkref %06llx %08x -> %08x\n", (unsigned long long)pa, (unsigned)before, (unsigned)after);
    return (after & 1u) != 0;
}
bool clocksAndResets() {
    // gcc-sm8550.c: USB30_PRIM_BCR 0x39000, QUSB2PHY_PRIM_BCR 0x12000; the core reset of dwc3_qcom_probe, the PHY reset of its init.
    volatile uint32_t *core = gGcc.va + 0x39000 / 4, *phy = gGcc.va + 0x12000 / 4;
    mark("bcr usb30 039000");
    *core = *core | 1u; barrier(); IODelay(20);
    *core = *core & ~1u; barrier();
    // Master 200 MHz from GPLL0 (source 1, half divider 5), mock UTMI 19.2 MHz from XO, as the dtsi assigns them.
    mark("rcg master 03902c");
    if (!rcg(0x3902c, 1u << 8 | 5u)) return false;
    mark("rcg mockutmi 039044");
    if (!rcg(0x39044, 0)) return false;
    mark("branch 039088 039018");
    if (!branch(0x39088) || !branch(0x39018)) return false;
    mark("branch 03908c 039024 039028");
    if (!branch(0x3908c) || !branch(0x39024) || !branch(0x39028)) return false;
    mark("tcsrcc usb2 clkref 1ff1118");
    if (!clkrefGate(0x01ff1118ull)) return false;
    mark("bcr qusb2phy 012000");
    *phy = *phy | 1u; barrier(); IODelay(150);
    *phy = *phy & ~1u; barrier();
    return true;
}
// SPMI arbiter v7 of this board (spmi@c400000, EE 0): one byte through this EE's channels.
struct Spmi {
    volatile uint32_t *core, *cnfg, *obs, *chn;
    uint64_t obsBytes, chnBytes;
    int apid;
};
uint32_t spmiCmd(uint8_t op, uint8_t addr) {return (uint32_t)op << 27 | (uint32_t)addr << 4;}
bool spmiWait(volatile uint32_t *ch) {
    for (unsigned i = 0; i < 100; ++i) {
        const uint32_t status = ch[2];
        if (status) return status == 1;
        IODelay(10);
    }
    return false;
}
bool spmiRead(const Spmi &s, uint8_t addr, uint8_t *out) {
    volatile uint32_t *ch = s.obs + (0x20u * (uint32_t)s.apid) / 4;
    ch[0] = spmiCmd(1, addr);
    if (!spmiWait(ch)) return false;
    *out = (uint8_t)ch[6];
    return true;
}
bool spmiWrite(const Spmi &s, uint8_t addr, uint8_t value) {
    volatile uint32_t *ch = s.chn + (0x1000u * (uint32_t)s.apid) / 4;
    ch[4] = value;
    barrier();
    ch[0] = spmiCmd(0, addr);
    return spmiWait(ch);
}
// init powers it and tunes it; every call sets the mode: eusb2_repeater_set_mode, host forces the 19.2 MHz clock enable on.
bool repeater(bool init, bool host) {
    Region core = {}, cnfg = {}, obs = {}, chn = {};
    mark("spmi map c400000");
    if (!mapRegion(core, 0xc400000, 0x3000) || !mapRegion(cnfg, 0xc42d000, 0x1000 * 2)) return false;
    Spmi s = {core.va, cnfg.va, nullptr, nullptr, 0, 0, -1};
    const uint32_t count = core.va[0x4 / 4] & 0x7ffu;
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        const uint32_t map = core.va[(0x2000 + 4 * i) / 4];
        if (((map >> 8) & 0xfffu) != 0x7fdu) continue;   // SID 7, peripheral 0xfd
        const int owner = (int)(cnfg.va[i] & 7u);
        if (s.apid < 0 || owner == 0) s.apid = (int)i;
    }
    IOLog("qcom-dwc3: mark spmi apid %d of %u\n", s.apid, (unsigned)count);
    bool ok = s.apid >= 0 && mapRegion(obs, 0xc440000, 0x20ull * (uint32_t)(s.apid + 1)) && mapRegion(chn, 0xc500000, 0x1000ull * (uint32_t)(s.apid + 1));
    if (ok) {
        s.obs = obs.va; s.chn = chn.va;
        if (init) {
            mark("eusb2 repeater enable");
            // eusb2_repeater_reset(enable) -> init(): out of reset, then the twelve tuning bytes of the mode in
            // use. This port is a device, so the device sequence is the one that matters (QcomEusb2Phy.h has the
            // DT lines); the host variant is here because the DT has it and a role change re-selects it.
            const uint8_t *tune = host ? kQcomRepeaterTuneHost : kQcomRepeaterTuneDevice;
            ok = spmiWrite(s, kQcomRepeaterEnable, kQcomRepeaterEnableBit);
            for (uint8_t i = 0; i < kQcomRepeaterTuneBytes; ++i)
                ok = ok && spmiWrite(s, (uint8_t)(kQcomRepeaterTuneFirst + i), tune[i]);
        }
        mark("eusb2 repeater 19p2m");
        const uint8_t force = host ? 1u << 6 : 0;   // F_CLK_19P2M_EN, V_CLK_19P2M_EN
        ok = ok && spmiWrite(s, kQcomRepeaterForceEn5, force) && spmiWrite(s, kQcomRepeaterForceVal5, force);
        if (init) {
            // eusb2_repeater_init() ends by polling +0x08 for bit 7, five times, 10-20 us apart, and treats a
            // timeout as non-fatal (it only logs). Here the count is kept, because "the repeater never came up"
            // and "it came up and the link is still wrong" are different rounds.
            uint8_t status = 0, tries = 0;
            for (; tries < 5; ++tries) {
                if (spmiRead(s, kQcomRepeaterStatus, &status) && (status & kQcomRepeaterOk)) {++tries; break;}
                IODelay(20);
            }
            native_usb_stats[8] = status | (uint32_t)tries << 8 | (uint32_t)s.apid << 16;
            ok = ok && (status & kQcomRepeaterOk);
        }
    }
    Region *regions[] = {&core, &cnfg, &obs, &chn};
    (void)regions;   // ml_io_map windows live for the boot
    return ok;
}
void phyInit() {
    // The table is twenty-odd writes into an eUSB2 PHY that the QUSB2PHY GDSC and its clock have to be running for.
    mark("eusb2 phy table");
    for (const auto &w : kQcomEusb2Init) {
        volatile uint32_t *reg = gPhy.va + w.offset / 4;
        *reg = (*reg & ~(uint32_t)w.mask) | w.value;
        barrier();
    }
}
// dwc3_qcom_select_utmi_clk: no PIPE clock until the QMP PHY runs.
void selectUtmiClock() {
    mark("utmi clock select");
    volatile uint32_t *cfg = gScratch.va + 0x8 / 4;
    *cfg = *cfg | 1u << 8; barrier(); IOSleep(1);
    *cfg = *cfg | 1u | 1u << 3; barrier(); IOSleep(1);
    *cfg = *cfg & ~(1u << 8); barrier();
}
void vbusOverride(bool on) {
    volatile uint32_t *ss = gScratch.va + 0x30 / 4, *hs = gScratch.va + 0x10 / 4;
    const uint32_t ssBits = 1u << 24, hsBits = 1u << 20 | 1u << 28;
    if (on) {*ss = *ss | ssBits; *hs = *hs | hsBits;} else {*ss = *ss & ~ssBits; *hs = *hs & ~hsBits;}
    barrier();
    native_usb_stats[10] = *hs;
}
// PS5169 retimer on i2c@a80000 (wrap 1 engine 0), address 0x28, reset GPIO 7 active low: the sequence of Linux ps5169.c for USB only.
uint32_t i2cRead(void *ctx, uint32_t reg) {return static_cast<volatile uint32_t *>(ctx)[reg / 4];}
void i2cWrite(void *ctx, uint32_t reg, uint32_t value) {static_cast<volatile uint32_t *>(ctx)[reg / 4] = value; barrier();}
bool i2cFinish(GeniI2c *bus, volatile uint32_t *se) {
    for (unsigned step = 0; step < 3; ++step) {
        for (unsigned i = 0; i < 2500; ++i) {
            const uint32_t status = se[GENI_M_IRQ_STATUS / 4];
            if (status) {
                if (status & GENI_M_TX_WATERMARK) se[GENI_TX_WATERMARK / 4] = 0;
                se[GENI_M_IRQ_CLEAR / 4] = status;
                const int result = geni_i2c_event(bus, status);
                if (result == GeniDone) return true;
                if (result == GeniFailed) return false;
            }
            IODelay(10);
        }
        const int result = geni_i2c_timeout(bus);
        if (result != GeniPending) return false;
    }
    return false;
}
bool retimerWrite(GeniI2c *bus, volatile uint32_t *se, uint8_t reg, uint8_t value) {
    return geni_i2c_start_write(bus, reg, value) == 0 && i2cFinish(bus, se);
}
bool retimerInit(bool reverse) {
    Region tlmm = {}, engine = {};
    bool ok = mapRegion(tlmm, kTlmm, 0x30000) && mapRegion(engine, 0xa80000, 0x4000);
    native_usb_stats[32] = 1u << 16;
    if (!ok) return false;
    // Reset released, then wait out the part's start (Linux: 20 ms).
    tlmm.va[(7 * 0x1000 + 4) / 4] = 1u << 1; barrier();
    tlmm.va[(7 * 0x1000) / 4] = 1u << 9; barrier();
    // qup1_se0 on GPIO 28, 29 with pull up, the serial engine clock and wrap 1's bus clocks on (BRANCH_HALT_VOTED).
    tlmm.va[(28 * 0x1000) / 4] = 3u | 1u << 2; tlmm.va[(29 * 0x1000) / 4] = 3u | 1u << 2; barrier();
    sm8550_gcc_update32(gGcc.va + 0x52008 / 4, 0, 1u << 22 | 1u << 18 | 1u << 19 | 1u << 20 | 1u << 21);
    IOSleep(20);
    native_usb_stats[32] = 2u << 16;
    // Nothing brought this engine up before: geni_se_init, the clock from the DFS table's XO level, SCL at 100 kHz from a 19.2 MHz clock
    // (geni_i2c_clk_map_19p2mhz; the retimer takes up to 400 kHz, so this stays in spec if the level runs faster).
    volatile uint32_t *se = engine.va;
    unsigned level = 8;
    for (unsigned i = 0; i < 8 && level == 8; ++i) {
        const uint32_t cfg = gGcc.va[(0x18010 + 0x1c + 4 * i) / 4];
        if (((cfg >> 8) & 7u) == 0 && (cfg & 31u) <= 1) level = i;
    }
    if (level == 8) {native_usb_stats[32] = 9u << 16; return false;}
    se[0x28 / 4] = se[0x28 / 4] | 0x7fu;                                        // CGC
    se[0xe30 / 4] = se[0xe30 / 4] | 0xfu;                                       // DMA clock gating
    se[0x24 / 4] = 0x7fu;                                                       // output control
    se[0x20 / 4] = 1u;                                                          // force default
    se[0xe18 / 4] = 0;                                                          // no GSI events
    se[0xe1c / 4] = se[0xe1c / 4] | 0xcu;                                       // M and S IRQ out of the FIFO block
    se[0x7c / 4] = level;
    se[GENI_CLK_CFG / 4] = 7u << 4 | 1u;
    se[GENI_SCL_COUNTERS / 4] = 10u << 20 | 12u << 10 | 26u;
    barrier();
    static GeniI2c bus;
    const GeniIo io = {i2cRead, i2cWrite, const_cast<uint32_t *>(engine.va)};
    uint32_t config = 0;
    if (geni_i2c_prepare(&bus, &io, 0x28, &config)) {native_usb_stats[32] = 3u << 16 | (config >> 24); return false;}
    uint32_t id = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (geni_i2c_start_read(&bus, (uint8_t)(0xac + i)) || !i2cFinish(&bus, engine.va)) {native_usb_stats[32] = (4u + i) << 16; return false;}
        id |= (uint32_t)bus.data << (8 * i);
    }
    native_usb_stats[32] = 6u << 16 | id;
    static const uint8_t table[][2] = {{0x8d, 1}, {0x90, 1}, {0x51, 0x87}, {0x50, 0x20}, {0x54, 0x11}, {0x5d, 0x66}, {0x52, 0x50}, {0x55, 0}, {0x56, 0},
        {0x57, 0}, {0x58, 0}, {0x59, 0}, {0x5a, 0}, {0x5b, 0}, {0x5e, 6}, {0x5f, 0}, {0x60, 0}, {0x61, 3}, {0x65, 0x40}, {0x66, 0}, {0x67, 3}, {0x75, 0x0c},
        {0x77, 0}, {0x78, 0x7c}};
    ok = retimerWrite(&bus, engine.va, 0x9d, 0x80);
    IOSleep(10);
    ok = ok && retimerWrite(&bus, engine.va, 0x9d, 0) && retimerWrite(&bus, engine.va, 0x40, 0x80) && retimerWrite(&bus, engine.va, 0xa0, 2);
    for (const auto &row : table) ok = ok && retimerWrite(&bus, engine.va, row[0], row[1]);
    ok = ok && retimerWrite(&bus, engine.va, 0x40, reverse ? 0xd0 : 0xc0);
    native_usb_stats[32] = (ok ? 7u : 8u) << 16 | id;
    return ok;
}
// QMP USB3/DP combo PHY at 0x88e8000 in USB 3 only mode: qmp_combo_com_init and qmp_combo_usb_power_on.
bool qmpInit(bool reverse) {
    Region phy = {};
    if (!mapRegion(phy, 0x088e8000, 0x3000)) return false;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(phy.va);
    auto rd = [&](uint32_t o) {return *reinterpret_cast<volatile uint32_t *>(base + o);};
    auto wr = [&](uint32_t o, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(base + o) = v; barrier();};
    volatile uint32_t *reset = gGcc.va + 0x50000 / 4, *common = gGcc.va + 0x50008 / 4;
    *reset = *reset | 1u; *common = *common | 1u; barrier(); IODelay(20);
    *reset = *reset & ~1u; *common = *common & ~1u; barrier();
    if (!rcg(0x39070, 0) || !branch(0x39060) || !branch(0x39064)) return false;
    sm8550_gcc_update32(gGcc.va + 0x3906c / 4, 3u, 0);                          // the pipe clock comes from the PHY
    sm8550_gcc_update32(gGcc.va + 0x39068 / 4, 0, 1u);
    wr(kQmpComPower, rd(kQmpComPower) | 1u);
    wr(kQmpComResetOvrd, rd(kQmpComResetOvrd) | 0xfu);
    wr(kQmpComTypec, 1u << 1 | (reverse ? 1u : 0));                             // software port select
    wr(kQmpComMode, 1u);                                                        // USB3_MODE
    wr(kQmpComResetOvrd, rd(kQmpComResetOvrd) & ~0xcu);
    wr(kQmpComSwi, rd(kQmpComSwi) & ~3u);
    wr(kQmpComReset, rd(kQmpComReset) & ~1u);
    wr(kQmpPowerDown, rd(kQmpPowerDown) | 1u);
    for (const auto &w : kQmpUsb3Init) wr(w.offset, w.value);
    wr(kQmpReset, rd(kQmpReset) & ~1u);
    wr(kQmpStart, rd(kQmpStart) | 3u);
    bool ok = false;
    for (unsigned i = 0; i < 1000 && !ok; ++i) {ok = !(rd(kQmpStatus) & 1u << 6); if (!ok) IODelay(200);}
    native_usb_stats[33] = rd(kQmpStatus);
    return ok;
}
// The USB 3 path, or false for USB 2 only with the UTMI clock in place of the PIPE clock (a USB 2 cable still works on the former).
bool superSpeedInit() {
    // Neither of the two pins sheng's board consults exists here (see kBoardHasRetimer / kBoardHasOrientationGpio
    // above). The orientation read is skipped rather than aimed at a guess: a wrong pin would report a wrong flip and
    // swap the SuperSpeed lanes, which is worse than not swapping them.
    const bool reverse = kBoardHasOrientationGpio ? (gTlmmOrientation & 1u) != 0 : false;
    native_usb_stats[34] = reverse;
    if (!kBoardHasRetimer) {
        mark("retimer none on this board");
    } else {
        mark("retimer ps5169");
        if (!retimerInit(reverse)) return false;
    }
    mark("qmp usb3 phy");
    return qmpInit(reverse);
}
Region gSmmu;
// Apple's controller driver does no cache maintenance: Apple's fabric is coherent. Here coherence comes from the SMMU's output
// attributes, as in Linux (dma-coherent, IOMMU_CACHE page tables): a stage 1 context with an identity map of DRAM in 1 GiB blocks,
// Normal write-back inner shareable. The bypass bank keeps device attributes and leaves this master non-coherent. No IOMapper:
// IODMACommand hands the physical address to the core, which the identity map turns back into the same address.
IOBufferMemoryDescriptor *gSmmuTable;
unsigned gSmmuBank, gSmmuPages;
constexpr uint32_t kCbSctlr = 0x0, kCbTcr2 = 0x10, kCbTtbr0 = 0x20, kCbTtbr1 = 0x28, kCbTcr = 0x30, kCbMair0 = 0x38, kCbMair1 = 0x3c, kCbFsr = 0x58,
    kCbTlbiAsid = 0x610, kCbTlbSync = 0x7f0, kCbTlbStatus = 0x7f4;
bool smmuIdentity() {
    mark("smmu map 15000000");
    if (!mapRegion(gSmmu, kSmmu, 0xd0000)) return false;
    AppsSmmuLock guard;
    if (!guard.ready()) return false;
    const uint32_t idr0 = gSmmu.va[0x20 / 4], idr1 = gSmmu.va[0x24 / 4];
    unsigned groups = idr0 & 255;
    if (groups > 128) groups = 128;
    const unsigned banks = idr1 & 255, pages = 1u << (((idr1 >> 28) & 7) + 1);
    if ((idr1 >> 31) || !groups || banks < 2 || banks > 128 || pages > 128 || (pages + banks) * 4096u > 0xd0000u) return false;
    uint64_t used[2] = {};
    unsigned slot = 0xffffffffu;
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = gSmmu.va[(0x800 + 4 * i) / 4];
        if (!(smr & 0x80000000u)) {if (slot == 0xffffffffu) slot = i; continue;}
        const uint32_t s2cr = gSmmu.va[(0xc00 + 4 * i) / 4];
        if (((smr ^ kSid) & 0xffff & ~((smr >> 16) & 0x7fff)) == 0) {IOLog("qcom-dwc3: stream already mapped %x %x\n", smr, s2cr); return false;}
        if (((s2cr >> 16) & 3) == 0) used[(s2cr & 127) / 64] |= 1ull << ((s2cr & 127) % 64);
    }
    IOLog("qcom-dwc3: mark smmu idr0 %08x idr1 %08x groups %u\n", (unsigned)idr0, (unsigned)idr1, groups);
    if (slot == 0xffffffffu) return false;
    // The last bank is the shared bypass bank. Take the highest bank no stream uses and no firmware left enabled.
    unsigned bank = 0xffffffffu;
    for (unsigned b = banks - 1; b-- > 0;) {
        if ((used[b / 64] >> (b % 64)) & 1) continue;
        if (gSmmu.va[(pages + b) * 1024] & 1u) continue;
        bank = b;
        break;
    }
    if (bank == 0xffffffffu) return false;
    IOLog("qcom-dwc3: mark smmu slot %u bank %u pages %u\n", slot, bank, pages);
    gSmmuTable = IOBufferMemoryDescriptor::withOptions((IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, 4096, 4096);
    if (!gSmmuTable || gSmmuTable->prepare() != kIOReturnSuccess) return false;
    uint64_t *table = static_cast<uint64_t *>(gSmmuTable->getBytesNoCopy());
    bzero(table, 4096);
    // T0SZ 28: 64 GiB, the walk starts at level 1. Blocks: AF, inner shareable, AP unprivileged read/write, attribute 0. Below 2 GiB
    // there is no DRAM, and a stray access should fault.
    for (uint64_t i = 2; i < 64; ++i) table[i] = i << 30 | 1ull << 10 | 3ull << 8 | 1ull << 6 | 1ull;
    for (uintptr_t at = reinterpret_cast<uintptr_t>(table); at < reinterpret_cast<uintptr_t>(table) + 4096; at += 64) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
    barrier();
    const uint64_t tablePa = gSmmuTable->getPhysicalSegment(0, nullptr, 0);
    volatile uint32_t *cb = gSmmu.va + (pages + bank) * 1024;
    gSmmu.va[(0x1800 + 4 * bank) / 4] = 1;                                      // CBA2R: AArch64
    gSmmu.va[(0x1000 + 4 * bank) / 4] = 1u << 16 | 0xfu << 12 | 3u << 8;        // CBAR: stage 1 with stage 2 bypass
    cb[kCbTcr2 / 4] = 2u | 7u << 15;                                            // 40-bit output, upstream sign extension
    cb[kCbTcr / 4] = 28u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23;            // walks cacheable inner shareable, 4 KiB, no TTBR1
    cb[kCbTtbr0 / 4] = (uint32_t)tablePa; cb[kCbTtbr0 / 4 + 1] = (uint32_t)(tablePa >> 32);
    cb[kCbTtbr1 / 4] = 0; cb[kCbTtbr1 / 4 + 1] = 0;
    cb[kCbMair0 / 4] = 0xff; cb[kCbMair1 / 4] = 0;                              // attribute 0: Normal write-back
    barrier();
    cb[kCbTlbiAsid / 4] = 0;
    cb[kCbTlbSync / 4] = 0;
    barrier();
    for (unsigned i = 0; i < 1000 && (cb[kCbTlbStatus / 4] & 1u); ++i) IODelay(10);
    cb[kCbFsr / 4] = cb[kCbFsr / 4];
    cb[kCbSctlr / 4] = 1u | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12;             // M, TRE, AFE, CFRE, ASIDPNE
    barrier();
    gSmmu.va[(0xc00 + 4 * slot) / 4] = bank;                                    // S2CR translate to the bank, then the match
    barrier();
    gSmmu.va[(0x800 + 4 * slot) / 4] = 0x80000000u | kSid;
    barrier();
    native_usb_stats[7] = slot | bank << 16;
    gSmmuBank = bank; gSmmuPages = pages;
    const bool ok = (cb[kCbSctlr / 4] & 1u) && (gSmmu.va[(0xc00 + 4 * slot) / 4] & 255) == bank &&
                    gSmmu.va[(0x800 + 4 * slot) / 4] == (0x80000000u | kSid);
    if (!ok) IOLog("qcom-dwc3: SMMU context readback %x %x %x\n", cb[kCbSctlr / 4], gSmmu.va[(0xc00 + 4 * slot) / 4], gSmmu.va[(0x800 + 4 * slot) / 4]);
    return ok;
}
}  // namespace

// A controller's provider: a register window and the core interrupt, in the IOService terms IOFilterInterruptEventSource uses. Device and
// host have a nub each and share the one line: it goes to the nub of the role the core is in.
class QcomDwc3Nub;
static QcomDwc3Nub *volatile gActiveNub;
static bool gLineAttached;
class QcomDwc3Nub : public IOService {
    OSDeclareDefaultStructors(QcomDwc3Nub)
    IOInterruptAction action = nullptr;
    OSObject *target = nullptr;
    void *refcon = nullptr;
    bool enabled = false;
    static void primary(void *) {
        QcomDwc3Nub *self = gActiveNub;
        native_usb_stats[11] = native_usb_stats[11] + 1;
        if (self && self->action) self->action(self->target, self->refcon, self, 0);
    }
    void unmask() {if (gActiveNub == this) armcpu_spi_mask(kIrq, !enabled);}
public:
    static void route(QcomDwc3Nub *nub) {
        armcpu_spi_mask(kIrq, true);
        gActiveNub = nub;
        __asm__ volatile("dmb ish" ::: "memory");
        if (nub) nub->unmask();
    }
    IOReturn getInterruptType(int source, int *type) override {
        if (source != 0 || !type) return kIOReturnNoInterrupt;
        *type = kIOInterruptTypeLevel;
        return kIOReturnSuccess;
    }
    IOReturn registerInterrupt(int source, OSObject *owner, IOInterruptAction handler, void *context) override {
        if (source != 0 || !handler) return kIOReturnNoInterrupt;
        if (action) return kIOReturnNoResources;
        target = owner; refcon = context; action = handler; enabled = false;
        __asm__ volatile("dmb ish" ::: "memory");
        if (!gLineAttached) {
            if (armcpu_spi_attach(kIrq, false, primary, nullptr, nullptr, nullptr) != kIOReturnSuccess) {action = nullptr; return kIOReturnNoResources;}
            gLineAttached = true;
        }
        unmask();
        return kIOReturnSuccess;
    }
    IOReturn unregisterInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        action = nullptr; target = nullptr; refcon = nullptr;
        return kIOReturnSuccess;
    }
    IOReturn enableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = true;
        unmask();
        return kIOReturnSuccess;
    }
    IOReturn disableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(QcomDwc3Nub, IOService)

// Whether the device stack may have the core: false while the core is in the host role or going there.
static volatile bool gDeviceStackAllowed = true;
class QcomDwc3Xdci : public AppleUSBXDCI {
    OSDeclareDefaultStructors(QcomDwc3Xdci)
    uint32_t read(uint32_t offset) {return (*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x450))[offset / 4];}
    void write(uint32_t offset, uint32_t value) {(*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x450))[offset / 4] = value; barrier();}
public:
    bool start(IOService *provider) override {
        // Two marks inside start(), because the 2026-11-22 crash was an instruction abort and "which of start()'s two halves"
        // decides the next round: the map of the provider's memory, or Apple's own start, which runs its reset sequence
        // through the XDCI_* overrides below.
        mark("xdci start: map provider");
        uint8_t *self = reinterpret_cast<uint8_t *>(this);
        IOMemoryMap *map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map) return false;
        *reinterpret_cast<IOMemoryMap **>(self + 0x448) = map;
        *reinterpret_cast<uint64_t *>(self + 0x450) = map->getVirtualAddress();
        IOLog("qcom-dwc3: mark xdci start: window=%llx\n", (unsigned long long)map->getVirtualAddress());
        mark("xdci start: super");
        return AppleUSBXDCI::start(provider);
    }
    // True keeps the family's already selected configuration list (IOUSBDeviceController builds it when this is false): Apple's SoC
    // classes answer from their IOPort transport object, and this class has none. The USB 3 descriptors follow the speed instead.
    bool XDCI_KEEP_CONFIGURATIONS() override {return false;}
    bool XDCI_TRANSPORT_ACTIVE() override {return true;}
    // The power the family asks for at each change, including when configd's description starts the stack: off while the host role has the core.
    uint32_t XDCI_DESIRED_POWER() override {return gDeviceStackAllowed ? AppleUSBXDCI::XDCI_DESIRED_POWER() : 0;}
    IOReturn XDCI_POWER_ON() override {
        native_usb_stats[12] = native_usb_stats[12] + 1;
        vbusOverride(true);
        native_usb_stats[15] = read(0x70c);
        return kIOReturnSuccess;
    }
    IOReturn XDCI_POWER_OFF() override {
        native_usb_stats[13] = native_usb_stats[13] + 1;
        vbusOverride(false);
        return kIOReturnSuccess;
    }
    // One event buffer and interrupter; the Linux driver has one as well. The scale and the periodic count are read in the reset.
    void XDCI_PARAMETERS() override {
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2628);
        fields[0] = 1; fields[1] = 0; fields[2] = 4; fields[3] = 4;
    }
    IOReturn XDCI_CONTROLLER_RESET() override {
        const IOReturn result = AppleUSBXDCI::XDCI_CONTROLLER_RESET();
        native_usb_stats[14] = (uint32_t)result;
        if (result != kIOReturnSuccess) return result;
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2628);
        // dwc3_core_setup_global_control keeps the power down scale the hardware resets to when there is no suspend clock.
        const uint32_t gctl = read(0x110);
        fields[1] = gctl >> 19;
        write(0x110, (gctl & ~(3u << 12)) | 2u << 12);
        // dwc3_gadget_setup_nump: RAM2 bytes less 24 for the event overhead and 16 per setup, in 1 KiB packets, at most 16.
        const uint32_t hw0 = read(0x140), hw6 = read(0x158), hw7 = read(0x15c);
        const uint32_t mdwidth = ((hw0 >> 8) & 0xffu) | ((hw6 >> 8) & 3u) << 8;
        const uint32_t ram2 = hw7 >> 16;
        uint32_t nump = ram2 * mdwidth / 8 > 40 ? (ram2 * mdwidth / 8 - 40) / 1024 : 1;
        fields[2] = nump > 16 ? 16 : nump ? nump : 1;
        native_usb_stats[2] = hw0; native_usb_stats[3] = read(0x14c); native_usb_stats[4] = hw7;
        // snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk: this board never suspends the eUSB2 PHY clock from the core.
        write(0x200, read(0x200) & ~(1u << 6 | 1u << 8));
        write(0x2c0, read(0x2c0) & ~(1u << 17));
        if (!gSuperSpeed) write(0x700, read(0x700) & ~7u);   // High speed
        native_usb_stats[16] = read(0x700); native_usb_stats[17] = read(0x110); native_usb_stats[18] = read(0x200);
        return result;
    }
    // Bandwidth accounting of Apple's fabric: nothing to vote for on this SoC, the SN2 vote covers the peak.
    // The family's power states: 4 starts the bus (the cable-connected path of Apple's SoC classes), 0 takes it off.
    IOReturn cable(bool connected) {return changePowerStateTo(connected ? 4 : 0);}
    uint32_t XDCI_PERIODIC_USAGE() override {return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x58c) != 0;}
    void XDCI_PERIODIC_SET(uint32_t) override {}
};
OSDefineMetaClassAndStructors(QcomDwc3Xdci, AppleUSBXDCI)

// Host mode: Apple's xHCI driver as it is on a PCI controller, with what Linux's dwc3 host glue requires of the core.
class QcomDwc3Xhci : public AppleUSBXHCI {
    OSDeclareDefaultStructors(QcomDwc3Xhci)
public:
    // Once the ports exist the family leaves the controller's power to them, and an idle port lets it fall to off: a halted controller reports
    // no attach, and what wakes it on Apple's hardware is the Type-C transport's cable report, which this class does not have. The role follows
    // the port manager, so this controller exists only with a peripheral on the cable: kPowerStateOn for as long as it exists.
    uint32_t XHCI_DESIRED_POWER() override {return 3;}
    // write-64-hi-lo-quirk: a 64-bit register is two 32-bit accesses, the high half written first.
    uint64_t XHCI_READ64(void *base, uint32_t offset) override {
        volatile uint32_t *at = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset);
        const uint64_t low = at[0];
        return low | (uint64_t)at[1] << 32;
    }
    void XHCI_WRITE64(void *base, uint32_t offset, uint64_t value) override {
        volatile uint32_t *at = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset);
        at[1] = (uint32_t)(value >> 32);
        barrier();
        at[0] = (uint32_t)value;
        barrier();
    }
};
OSDefineMetaClassAndStructors(QcomDwc3Xhci, AppleUSBXHCI)

namespace {
bool bringUp() {
    stage(1);
    if (!mapRegion(gGcc, kGcc, 0x1f0000) || !mapRegion(gTcsrMap, kTcsr, 0x1000) || !mapRegion(gPhy, kPhy, 0x1000) ||
        !mapRegion(gScratch, kQscratch & ~0xfffull, 0x1000)) {stage(-1); return false;}
    gScratch.va += (kQscratch & 0xfff) / 4;
    stage(2);
    // A rail this driver could not vote does not end the probe: the bootloader powered the USB core to hand this image over, so the
    // core, the PHY and the wrapper are very likely still up, and the stages below say far more than a boot that stops here. The
    // `rails votes=... completed=...` line is the record of what was missing, and the next stage's own verdict follows either way.
    //
    // `usb-rails=0` skips the whole vote. It exists because the two are separable questions -- "does the RSC take our votes" and
    // "does the controller come up" -- and the 2026-11-21 boots showed the second one can be answered without the first: with the
    // vote skipped, both USB30 GDSCs collapsed and read back powered (stage 3 passed) on the power the bootloader had already left.
    unsigned rails = 1;
    if (PE_parse_boot_argn("usb-rails", &rails, sizeof rails) && !rails) {
        IOLog("qcom-dwc3: rails skipped (usb-rails=0), bootloader power state only\n");
    } else if (!powerRails()) {
        IOLog("qcom-dwc3: rails not voted here, carrying on with what the bootloader left\n");
    }
    stage(3);
    if (!gdsc(0x50018, 6) || !gdsc(0x39004, 5)) {stage(-3); return false;}
    stage(4);
    if (!clocksAndResets()) {stage(-4); return false;}
    stage(5);
    if (!smmuIdentity()) {stage(-5); return false;}
    stage(6);
    if (!repeater(true, false)) {stage(-6); return false;}
    stage(7);
    phyInit();
    IOSleep(2);
    gSuperSpeed = superSpeedInit();
    if (!gSuperSpeed) selectUtmiClock();
    stage(8);
    mark("apple window a6c000");
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000)) {stage(-8); return false;}
    const uint32_t id = window.va[0x120 / 4];
    native_usb_stats[1] = id;
    IOLog("qcom-dwc3: mark gsnpsid %08x\n", (unsigned)id);
    (void)window;
    if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u && (id & 0xffff0000u) != 0x33320000u) {stage(-9); return false;}
    stage(9);
    return true;
}
}  // namespace
QcomDwc3Xdci *gController;
int gVbusOn = -1, gVbusApplied = -1;
QcomUcsi gUcsi;
// One role at a time, changed on one thread: the system's start and stop paths block, and the battery work loop reports the cable.
enum Role {kRoleNone, kRoleDevice, kRoleHost};
IOLock *gRoleLock;
int gRoleWanted, gRoleApplied, gCoreRole = kRoleDevice;
bool gForceHost;
Region gGlobals, gXhciRegs;
QcomDwc3Nub *gDeviceNub, *gHostNub;
QcomDwc3Xhci *gHost;
void wantRole(int role) {
    if (!gRoleLock) return;
    IOLockLock(gRoleLock);
    gRoleWanted = role;
    IOLockWakeup(gRoleLock, &gRoleWanted, false);
    IOLockUnlock(gRoleLock);
}
// What is known now, by ucsi_partner_change: the port manager when it answered, which tells a USB host from a charger and a peripheral; the
// charger firmware's online value until then, or if UCSI does not come up.
void cableChanged() {
    int role = gVbusOn > 0 ? kRoleDevice : kRoleNone;
    if (gUcsi.status_valid) {
        role = kRoleNone;
        if (gUcsi.connected && gUcsi.usb) {
            if (gUcsi.partner == kUcsiPartnerDfp) role = kRoleDevice;
            else if (gUcsi.partner == kUcsiPartnerUfp || gUcsi.partner == kUcsiPartnerCableUfp) role = kRoleHost;
        }
    }
    wantRole(gForceHost ? kRoleHost : role);
}
void forceHostThread(void *seconds, wait_result_t) {
    IOSleep((uint32_t)reinterpret_cast<uintptr_t>(seconds) * 1000);
    gForceHost = true;
    cableChanged();
    // usb-back=<seconds in the host role>: then what the cable says, for an unattended run of the way back.
    unsigned back = 0;
    if (PE_parse_boot_argn("usb-back", &back, sizeof back) && back) {
        IOSleep(back * 1000);
        gForceHost = false;
        cableChanged();
    }
}
void leaveRole(int role) {
    if (role == kRoleDevice) {
        IOService::getResourceService()->removeProperty("AppleUSBCableType");
        // The family stops the bus through XDCI_POWER_OFF; with no description from configd yet the bus was never on and it does not come.
        const uint32_t before = native_usb_stats[13];
        native_usb_stats[20] = (uint32_t)gController->cable(false);
        for (unsigned i = 0; i < 200 && native_usb_stats[13] == before; ++i) IOSleep(10);
    } else if (role == kRoleHost && gHostNub) {
        // A removed PCI controller's path: the family stops its devices and the controller.
        gHostNub->terminate(kIOServiceSynchronous);
        gHostNub->release();
        gHostNub = nullptr; gHost = nullptr;
        QcomDwc3Nub::route(nullptr);
    }
}
// __dwc3_set_mode: a core soft reset between the roles, then the port capability.
void setCoreRole(int role) {
    if (gCoreRole == role) return;
    volatile uint32_t *gctl = gGlobals.va + 0x110 / 4;
    QcomDwc3Nub::route(nullptr);
    *gctl = *gctl | 1u << 11;
    barrier();
    IOSleep(100);
    *gctl = *gctl & ~(1u << 11);
    barrier();
    *gctl = (*gctl & ~(3u << 12)) | (role == kRoleHost ? 1u : 2u) << 12;
    barrier();
    // snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk, as in the device reset.
    gGlobals.va[0x200 / 4] = gGlobals.va[0x200 / 4] & ~(1u << 6 | 1u << 8);
    gGlobals.va[0x2c0 / 4] = gGlobals.va[0x2c0 / 4] & ~(1u << 17);
    barrier();
    gCoreRole = role;
    gDeviceStackAllowed = role == kRoleDevice;
    repeater(false, role == kRoleHost);
}
// qmp_combo_typec_switch_set and ps5169: the lanes follow the plug. The GPIO means something once a cable is in.
void followOrientation() {
    if (!kBoardHasOrientationGpio) return;   // this board declares no orientation pin; see kBoardHasRetimer
    Region tlmm = {};
    if (!mapRegion(tlmm, kTlmm, 0x10000)) return;
    const uint32_t reverse = tlmm.va[(11 * 0x1000 + 4) / 4] & 1u;
    (void)tlmm;   // an ml_io_map window lives for the boot; mapRegion leaves Region::map null, so nothing is released here
    if (gSuperSpeed && reverse != native_usb_stats[34]) {
        gSuperSpeed = superSpeedInit();
        if (!gSuperSpeed) selectUtmiClock();
    }
}
void enterHost() {
    QcomDwc3Nub *nub = OSTypeAlloc(QcomDwc3Nub);
    // DWC3_XHCI_REGS_END: the host registers below the global ones.
    IODeviceMemory *range = IODeviceMemory::withRange(kCore, 0x8000);
    OSArray *ranges = range ? OSArray::withObjects((const OSObject **)&range, 1) : nullptr;
    OSData *zero = OSData::withBytes("\0\0\0\0", 4);
    bool done = false;
    if (nub && nub->init() && ranges && zero) {
        nub->setDeviceMemory(ranges);
        nub->setName("usb-host0");
        nub->setProperty("bus-number", zero);
        if (nub->attach(IOService::getPlatform())) {
            nub->registerService();
            QcomDwc3Nub::route(nub);
            QcomDwc3Xhci *host = OSTypeAlloc(QcomDwc3Xhci);
            if (host && host->init() && host->attach(nub)) {
                done = host->start(nub);
                if (!done) host->detach(nub);
                else gHost = host;
            }
            OSSafeReleaseNULL(host);
        }
    }
    native_usb_stats[46] = (native_usb_stats[46] & 0xffff0000u) | (done ? 1u : 2u);
    OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(range); OSSafeReleaseNULL(zero);
    if (done) gHostNub = nub;
    else if (nub) {QcomDwc3Nub::route(nullptr); nub->terminate(kIOServiceSynchronous); nub->release();}
}
void enterRole(int role) {
    if (role == kRoleNone) return;
    followOrientation();
    setCoreRole(role);
    if (role == kRoleHost) {
        vbusOverride(false);
        enterHost();
    } else {
        QcomDwc3Nub::route(gDeviceNub);
        // A USB host on the cable: lockdownd's launch event, then the family goes on the bus when configd's description arrives.
        IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
        native_usb_stats[20] = (uint32_t)gController->cable(true);
    }
}
void hostStats() {
    if (gCoreRole != kRoleHost) return;
    const uint32_t cap = gXhciRegs.va[0];
    const volatile uint32_t *op = gXhciRegs.va + (cap & 0xffu) / 4;
    native_usb_stats[47] = op[0]; native_usb_stats[48] = op[1];
    native_usb_stats[49] = op[0x400 / 4]; native_usb_stats[50] = op[0x410 / 4];
    native_usb_stats[51] = gXhciRegs.va[1];
    if (gHost) native_usb_stats[46] = (native_usb_stats[46] & 0xffff00ffu) | (uint32_t)gHost->getPowerState() << 8;
    // What the system's stack enumerated: how many devices now, the last one's vendor and product (kept after it leaves).
    OSDictionary *match = IOService::serviceMatching("IOUSBHostDevice");
    OSIterator *devices = match ? IOService::getMatchingServices(match) : nullptr;
    uint32_t count = 0;
    while (OSObject *object = devices ? devices->getNextObject() : nullptr) {
        ++count;
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, object);
        OSNumber *vendor = entry ? OSDynamicCast(OSNumber, entry->getProperty("idVendor")) : nullptr;
        OSNumber *product = entry ? OSDynamicCast(OSNumber, entry->getProperty("idProduct")) : nullptr;
        if (vendor && product) native_usb_stats[53] = vendor->unsigned32BitValue() << 16 | (product->unsigned32BitValue() & 0xffffu);
    }
    native_usb_stats[52] = count;
    OSSafeReleaseNULL(devices); OSSafeReleaseNULL(match);
}
void roleThread(void *, wait_result_t) {
    for (;;) {
        IOLockLock(gRoleLock);
        while (gRoleWanted == gRoleApplied) {
            if (gRoleApplied != kRoleHost) IOLockSleep(gRoleLock, &gRoleWanted, THREAD_UNINT);
            else {
                uint64_t deadline;
                clock_interval_to_deadline(1, kSecondScale, &deadline);
                IOLockSleepDeadline(gRoleLock, &gRoleWanted, deadline, THREAD_UNINT);
                hostStats();
            }
        }
        const int role = gRoleWanted;
        IOLockUnlock(gRoleLock);
        if (role == kRoleHost) gDeviceStackAllowed = false;
        leaveRole(gRoleApplied);
        gRoleApplied = kRoleNone;
        enterRole(role);
        gRoleApplied = role;
        native_usb_stats[44] = (uint32_t)role | (uint32_t)gRoleWanted << 8 | (uint32_t)gCoreRole << 16;
        native_usb_stats[45] = native_usb_stats[45] + 1;
        native_usb_stats[35] = ((native_usb_stats[35] & 0xffffu) + 1) | (uint32_t)(role == kRoleDevice) << 16;
    }
}
extern "C" void qcom_dwc3_vbus(int online) {
    gVbusOn = online != 0;
    cableChanged();
}
extern "C" void qcom_dwc3_host_load(int result) {native_usb_stats[46] = (uint32_t)result << 16;}
// The USB-C owner on the PMIC channel; the board's battery service owns the channel and its work loop, and calls these there.
extern "C" uint32_t qcom_ucsi_start(uint8_t *out) {return ucsi_start(&gUcsi, out);}
static void ucsiStats() {
    native_usb_stats[40] = (uint32_t)gUcsi.state | gUcsi.version << 16;
    native_usb_stats[41] = gUcsi.status;
    native_usb_stats[42] = gUcsi.cci;
    native_usb_stats[43] = gUcsi.changes | gUcsi.errors << 16;
}
extern "C" uint32_t qcom_ucsi_receive(const uint8_t *data, uint32_t length, uint8_t *out) {
    const uint32_t n = ucsi_receive(&gUcsi, data, length, out);
    ucsiStats();
    if (gUcsi.state == kUcsiReady && !n) cableChanged();
    return n;
}
extern "C" uint32_t qcom_ucsi_tick(uint8_t *out) {
    const uint32_t n = ucsi_tick(&gUcsi, out);
    ucsiStats();
    return n;
}
extern "C" bool qcom_ucsi_polling(void) {return gUcsi.polling != 0;}
extern "C" uint32_t qcom_ucsi_request_data_host(uint8_t *out) {
    const uint32_t bytes = ucsi_request_data_host(&gUcsi, out);
    ucsiStats();
    return bytes;
}
extern "C" int qcom_ucsi_data_host(void) {return ucsi_data_host(&gUcsi);}
extern "C" bool qcom_ucsi_connection(uint32_t *generation, uint32_t *status) {
    if (!gUcsi.status_valid || gUcsi.state == kUcsiFailed) return false;
    *generation = gUcsi.connection_generation;
    *status = gUcsi.status;
    return true;
}
// The hardware, before the classes exist: a failure leaves the system running with USB off.
extern "C" bool qcom_dwc3_bring_up(void) {return bringUp();}
// After the module's metaclasses are registered: publish the provider and start the controller.
// For bring-up (usb=2): what the core and the SMMU see while the system stack runs, sampled about every 5 ms for the link state.
// cycle (usb=3) also takes the controller off the bus and back once, the path a cable removal runs, for an unattended host check.
extern "C" void qcom_dwc3_diag(bool cycle) {
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000) || !gSmmu.va) return;
    const uint32_t pages = gSmmuPages, bank = gSmmuBank;
    for (uint32_t tick = 0;; ++tick) {
        const uint32_t ltssm = window.va[0x164 / 4];
        native_usb_stats[36] = native_usb_stats[36] | 1u << ((ltssm >> 22) & 15u);
        native_usb_stats[37] = ltssm;
        if (!(tick % 400)) {
            native_usb_stats[22] = window.va[0x100 / 4]; native_usb_stats[23] = window.va[0x104 / 4]; native_usb_stats[24] = window.va[0x11c / 4];
            native_usb_stats[25] = gSmmu.va[0x48 / 4];
            native_usb_stats[26] = window.va[0x70c / 4]; native_usb_stats[27] = window.va[0x720 / 4]; native_usb_stats[28] = window.va[0x40c / 4];
            native_usb_stats[29] = window.va[0x80c / 4]; native_usb_stats[30] = gSmmu.va[(pages + bank) * 1024 + 0x58 / 4]; native_usb_stats[31] = window.va[0x408 / 4];
            native_usb_stats[38] = window.va[0x2c0 / 4];
        }
        if (cycle && tick == 16000) {wantRole(kRoleNone); native_usb_stats[39] = 1;}
        if (cycle && tick == 18000) {wantRole(kRoleDevice); native_usb_stats[39] = 2;}
        IOSleep(5);
    }
}
// Live sample of the device-mode registers the enumeration has to move, for the printed beats: DSTS (0x70c),
// DCFG (0x704), GCTL (0x110) and GUSB2PHYCFG (0x200) in the window Apple's class is given, plus the link state
// from DSTS bits 5:0. Nothing here writes hardware, and nothing touches a window that publish() has not mapped,
// so the reporter can call it before the stack exists.
extern "C" void qcom_dwc3_sample(void)
{
    if (!gGlobals.va) return;
    const uint32_t dsts = gGlobals.va[0x70c / 4], dcfg = gGlobals.va[0x704 / 4];
    native_usb_stats[15] = dsts;
    native_usb_stats[16] = dcfg;
    native_usb_stats[17] = gGlobals.va[0x110 / 4];
    native_usb_stats[18] = gGlobals.va[0x200 / 4];
    native_usb_stats[26] = dsts;                          // the same register the diag loop calls DSTS
    // DSTS bits 21:18 are USBLNKST and bits 3:0 the connected speed (dwc3's DWC3_DSTS_USBLNKST /
    // DWC3_DSTS_CONNECTSPD). [36] accumulates every link state this boot has seen, so one photograph says
    // whether the link ever left 0 (OnDisconnect) or reached Polling/Configured.
    native_usb_stats[36] = native_usb_stats[36] | 1u << ((dsts >> 18) & 0xf);
    native_usb_stats[37] = dsts;
}

extern "C" void qcom_dwc3_publish(void) {
    do {
        OSData *zero = OSData::withBytes("\0\0\0\0", 1);
        QcomDwc3Nub *nub = OSTypeAlloc(QcomDwc3Nub);
        IOService *platform = IOService::getPlatform();
        IODeviceMemory *range = IODeviceMemory::withRange(kAppleWindow, 0x1000);
        OSArray *ranges = range ? OSArray::withObjects((const OSObject **)&range, 1) : nullptr;
        if (!nub || !nub->init() || !ranges || !zero) {stage(-12); break;}
        nub->setDeviceMemory(ranges);
        nub->setName("usb-drd0");
        nub->setProperty("bus-number", zero);
        nub->setProperty("configuration-string", (void *)"standardMuxOnly", 16);
        // snps,dis-u1-entry-quirk, snps,dis-u2-entry-quirk.
        nub->setProperty("u1-init-disable", true);
        nub->setProperty("u2-init-disable", true);
        if (!nub->attach(platform)) {stage(-13); break;}
        nub->registerService();
        OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(range); OSSafeReleaseNULL(zero);
        stage(10);
        // Four separate calls, four marks. The 2026-11-22 usb=4 boot got `xdci load -> 0` and this `stage 10` and then died
        // in an instruction abort (esr 0x86000006 = instruction abort, same EL, level 2 translation fault; pc 0xfffffff00f7da6a0,
        // past the end of the kernel's text), which is a jump through a pointer that is not code. That is one of these four
        // calls and nothing else, so each one now names itself: alloc (the metaclass), init, attach, start. The metaclass and
        // the object's isa are printed too, because "the class we registered" and "the class the allocator used" have to be
        // the same object for any of the four to be real calls.
        // The two panels of 2026-11-22 both stopped between "mark xdci alloc" and the isa print, so the fault is inside
        // OSTypeAlloc() itself -- before any of the four calls, and before any virtual dispatch of ours. OSTypeAlloc is
        // one indirect call into libkern, so it is split here into the parts that can be wrong, cheapest first:
        //   1. what the metaclass believes about itself (name, class size, superclass) -- ours and the base's;
        //   2. a control: allocate one of this port's own plain IOService subclasses, which already worked at stage 10;
        //   3. the base class on its own, with none of our overrides in it;
        //   4. ours.
        // Whichever line is last is the one that is wrong, and (2) versus (3) separates "the allocator is broken now"
        // from "Apple's class cannot be constructed through this ABI" from "our subclass cannot".
        IOLog("qcom-dwc3: mark xdci meta self=%p name=%s size=%u super=%p\n", (void *)&QcomDwc3Xdci::gMetaClass,
              QcomDwc3Xdci::gMetaClass.getClassName(), (unsigned)QcomDwc3Xdci::gMetaClass.getClassSize(),
              (void *)QcomDwc3Xdci::gMetaClass.getSuperClass());
        // The base metaclass is the first thing this driver touches that lives in Apple's kext rather than in this
        // kernel's own objects: `&AppleUSBXDCI::gMetaClass` is a symbol the prelink had to bind, and it is also what
        // the bound constructor installs into the object. Printing it and then reading through it separates "the
        // binding is right" (the reads answer) from "the binding points outside the image" (a data abort here,
        // before anything of ours runs).
        IOLog("qcom-dwc3: mark xdci base meta at %p\n", (void *)&AppleUSBXDCI::gMetaClass);
        const OSMetaClass *baseMeta = &AppleUSBXDCI::gMetaClass;
        IOLog("qcom-dwc3: mark xdci base meta size=%u super=%p name=%s\n", (unsigned)baseMeta->getClassSize(),
              (void *)baseMeta->getSuperClass(), baseMeta->getClassName());
        IOLog("qcom-dwc3: mark xdci base size=%u super=%p isa_size=%u\n", (unsigned)AppleUSBXDCI::gMetaClass.getClassSize(),
              (void *)AppleUSBXDCI::gMetaClass.getSuperClass(), (unsigned)sizeof(QcomDwc3Xdci));
        mark("xdci ctl alloc");
        QcomDwc3Nub *control = OSTypeAlloc(QcomDwc3Nub);
        IOLog("qcom-dwc3: mark xdci ctl alloc -> %p\n", (void *)control);
        // The base-class probe that used to sit here is gone: instantiating an abstract class from
        // libkern's side pulls Apple's vtable and destructor into this archive for no diagnostic value
        // (the metaclass reads above already test the binding), and it is the only construct in this
        // round that was new to the link. See the bisect note in docs/STATE.md.
        mark("xdci alloc");
        QcomDwc3Xdci *controller = OSTypeAlloc(QcomDwc3Xdci);
        if (controller) IOLog("qcom-dwc3: mark xdci isa=%p\n", *(void **)controller);
        mark("xdci init");
        if (!controller || !controller->init()) {stage(-14); break;}
        mark("xdci attach");
        if (!controller->attach(nub)) {stage(-15); break;}
        mark("xdci start");
        const bool started = controller->start(nub);
        native_usb_stats[19] = started;
        if (!started) {stage(-16); break;}
        stage(11);
        gController = controller;
        gDeviceNub = nub;
        gActiveNub = nub;
        // The core stays in the device role until a role asks otherwise: the controller's reset set it.
        thread_t thread = nullptr;
        // usb-host=<seconds>: the host role that long after this point, whatever is on the cable, to bring the controller up with nothing
        // plugged in and to run the change of role.
        unsigned forced = 0;
        PE_parse_boot_argn("usb-host", &forced, sizeof forced);
        gRoleLock = IOLockAlloc();
        if (!gRoleLock || !mapRegion(gGlobals, kAppleWindow, 0x1000) || !mapRegion(gXhciRegs, kCore, 0x1000) ||
            kernel_thread_start(roleThread, nullptr, &thread) != KERN_SUCCESS) {stage(-17); break;}
        thread_deallocate(thread);
        if (forced && kernel_thread_start(forceHostThread, reinterpret_cast<void *>((uintptr_t)forced), &thread) == KERN_SUCCESS) thread_deallocate(thread);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        // The cable as the charger firmware reports it; it may have answered before the controller existed. With neither yet, the device
        // role: a host needs no action from this end to be seen.
        if (gVbusOn < 0 && !gUcsi.status_valid) gVbusOn = 1;
        cableChanged();
        stage(12);
    } while (false);
}
