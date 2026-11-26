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
#ifdef QCOM_DWC3_UKE_USB2
#include "UkeUsbBoard.h"
#else
#include "AppsSmmuLock.h"
#endif
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#ifdef QCOM_DWC3_UKE_USB2
#include <IOKit/IOTimerEventSource.h>
#endif
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#ifdef QCOM_DWC3_UKE_USB2
extern "C" void uke_usb_pm_snapshot(IOService *);
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" boolean_t pmap_valid_address(uint64_t pa);
extern "C" vm_offset_t ml_vtophys(vm_offset_t va);
#endif
extern "C" {
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
int armcpu_spi_mask(uint32_t, bool);
uint64_t armcpu_spi_count(uint32_t);
bool sm8550_gcc_update32(volatile uint32_t *, uint32_t, uint32_t);
// [0] stage (negative: the step that failed), [1] GSNPSID, [2] GUID-free hardware parameter 0, [3] parameter 3, [4] parameter 7,
// [5] GDSC USB30, [6] GDSC PHY, [7] SMMU slot | bank << 16, [8] repeater status, [9] RPMh rails found, [10] VBUS override state,
// [11] interrupts, [12] powerOn calls, [13] powerOff calls, [14] controllerReset result, [15] DSTS at power on, [16] DCFG after
// reset, [17] GCTL after reset, [18] GUSB2PHYCFG after reset, [19] start result, [20] power state request result, [21] kext load result,
// with usb=2, every 2 s: [22] GSBUSCFG0, [23] GSBUSCFG1, [24] GUCTL1, [25] SMMU global fault, [26] DSTS, [27] DALEPENA,
// [28] GEVNTCOUNT, [29] DEPCMD of EP0 OUT, [30] bypass bank fault status, [31] GEVNTSIZ.
// [32] retimer: chip id | stage << 16, [33] QMP PCS status, [34] orientation GPIO, [35] VBUS changes | last value << 16,
// [44] role applied | wanted << 8 | core << 16, [45] role changes, [46] xHCI load << 16 | start, [47] USBCMD, [48] USBSTS, [49] [50] PORTSC 1 and 2,
// [51] HCSPARAMS1,
// [40] UCSI state | version << 16, [41] UCSI connector status, [42] CCI, [43] changes | errors << 16,
// [36] bit mask of the SuperSpeed link states seen (GDBGLTSSM bits 25:22), [37] GDBGLTSSM, [38] GUSB3PIPECTL, [39] cable test steps.
// [52] QSCRATCH SS_PHY_CTRL readback, [53] VBUS/SS override validation after reset.
volatile uint32_t native_usb_stats[54];
}
namespace {
// The DWC3 global, device and OTG registers in the window Apple's class addresses: base + 0xc000.
constexpr uint64_t kCore = 0x0a600000, kAppleWindow = kCore + 0xc000, kQscratch = kCore + 0xf8800, kSmmu = 0x15000000;
constexpr uint32_t kIrq = 133 + 32, kSid = 0x40;
#ifndef QCOM_DWC3_UKE_USB2
constexpr uint64_t kGcc = 0x100000, kTcsr = 0x1fd5000, kPhy = 0x088e3000, kRsc = 0x17a20000, kCmdDb = 0x81c60000;
constexpr uint32_t kCmdDbSize = 0x20000;
constexpr unsigned kOwnedTcs = 2;
#endif
struct Region {IOMemoryMap *map; volatile uint32_t *va;};
Region gScratch, gXhciRegs;
#ifndef QCOM_DWC3_UKE_USB2
Region gGcc, gPhy, gTcsrMap;
#endif
bool gSuperSpeed;
#ifdef QCOM_DWC3_UKE_USB2
IOLock *gUkeUsbRegisters;
bool gUkeHostCore;
class UkeUsbRegistersGuard {
public:
    UkeUsbRegistersGuard() {if (gUkeUsbRegisters) IOLockLock(gUkeUsbRegisters);}
    ~UkeUsbRegistersGuard() {if (gUkeUsbRegisters) IOLockUnlock(gUkeUsbRegisters);}
};
#endif
void stage(int value) {native_usb_stats[0] = (uint32_t)value;}
bool mapRegion(Region &r, uint64_t pa, uint64_t bytes) {
    IODeviceMemory *d = IODeviceMemory::withRange(pa, bytes);
    r.map = d ? d->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(d);
    r.va = r.map ? reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()) : nullptr;
    return r.va != nullptr;
}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
#ifdef QCOM_DWC3_UKE_USB2
uint64_t readUkeRegisterPair(const volatile uint32_t *address) {
    const uint32_t low = address[0];
    return low | (uint64_t)address[1] << 32;
}

// The Uke USB SMMU context maps IOVAs directly to RAM. Sample only small,
// aligned records from a valid page through the existing cached aperture.
template <unsigned Words>
bool readUkeDmaWords(uint64_t pa, uint32_t (&words)[Words]) {
    if (pa < (2ull << 30) || pa >= (64ull << 30) || (pa & 3u) ||
        (pa & 0xfffu) + sizeof(words) > 4096 ||
        !pmap_valid_address(pa)) return false;
    const auto *record = reinterpret_cast<const volatile uint32_t *>(phystokv(pa));
    if (!record) return false;
    for (unsigned i = 0; i < Words; ++i) words[i] = record[i];
    return true;
}
#endif
#ifndef QCOM_DWC3_UKE_USB2
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
    if (ok) {
        for (uint32_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = dbw.va[i / 4]; memcpy(db + i, &w, 4);}
        const QcomRscLayout layout = qcom_rsc_layout(rsc.va[0]);
        AppsSmmuLock guard;
        auto send = [&](uint32_t address, uint32_t value) {
            QcomRpmhWrite writes[16];
            const unsigned n = qcom_rpmh_active_request(&layout, kOwnedTcs, &address, &value, 1, writes, 16);
            for (unsigned i = 0; i < n; ++i) rsc.va[(0xd00 + writes[i].offset) / 4] = writes[i].value;
            IOSleep(2);
            const unsigned m = qcom_rpmh_finish(&layout, kOwnedTcs, writes, 16);
            for (unsigned i = 0; i < m; ++i) rsc.va[(0xd00 + writes[i].offset) / 4] = writes[i].value;
        };
        static const struct {const char *name; uint32_t mv;} rails[] = {
            {"ldoe1", 880}, {"ldoe3", 1200}, {"ldob15", 1800}, {"ldob5", 3104}, {"ldof3", 912}};
        for (unsigned i = 0; i < sizeof rails / sizeof rails[0] && ok; ++i) {
            QcomRpmhResource res;
            if (qcom_cmd_db_find(db, kCmdDbSize, rails[i].name, &res)) {ok = false; break;}
            send(res.address + 8, 7);
            send(res.address, rails[i].mv);
            send(res.address + 4, 1);
            native_usb_stats[9] = native_usb_stats[9] | (1u << i);
        }
        QcomRpmhResource res;
        if (ok && !qcom_cmd_db_find(db, kCmdDbSize, "xo.lvl", &res)) send(res.address, 3); else ok = false;
        // SN2 carries qnm_aggre1_noc: vote = ceil(ceil(kBps * width / buswidth) * 1000 / unit), buswidth 16, 2500 MB/s peak.
        if (ok && !qcom_cmd_db_find(db, kCmdDbSize, "SN2", &res)) {
            uint32_t vote = 0x400;
            if (res.level_count >= 3 && (res.levels[0] | (uint32_t)res.levels[1] << 16) && res.levels[2]) {
                const uint64_t unit = res.levels[0] | (uint32_t)res.levels[1] << 16;
                const uint64_t x = (((2500000ull * res.levels[2] + 15) / 16) * 1000 + unit - 1) / unit;
                vote = x < 1 ? 1 : x > 0x3fff ? 0x3fff : (uint32_t)x;
            }
            send(res.address, 1u << 30 | 1u << 29 | vote << 14 | vote);
            native_usb_stats[9] = native_usb_stats[9] | 0x80000000u;
        } else ok = false;
    }
    if (dbw.map) dbw.map->release();
    if (rsc.map) rsc.map->release();
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
bool clocksAndResets() {
    // gcc-sm8550.c: USB30_PRIM_BCR 0x39000, QUSB2PHY_PRIM_BCR 0x12000; the core reset of dwc3_qcom_probe, the PHY reset of its init.
    volatile uint32_t *core = gGcc.va + 0x39000 / 4, *phy = gGcc.va + 0x12000 / 4;
    *core = *core | 1u; barrier(); IODelay(20);
    *core = *core & ~1u; barrier();
    // Master 200 MHz from GPLL0 (source 1, half divider 5), mock UTMI 19.2 MHz from XO, as the dtsi assigns them.
    if (!rcg(0x3902c, 1u << 8 | 5u) || !rcg(0x39044, 0)) return false;
    if (!branch(0x39088) || !branch(0x39018) || !branch(0x3908c) || !branch(0x39024) || !branch(0x39028)) return false;
    gTcsrMap.va[0x118 / 4] = gTcsrMap.va[0x118 / 4] | 1u;
    gTcsrMap.va[0x108 / 4] = gTcsrMap.va[0x108 / 4] | 1u;
    barrier();
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
    if (!mapRegion(core, 0xc400000, 0x3000) || !mapRegion(cnfg, 0xc42d000, 0x1000 * 2)) return false;
    Spmi s = {core.va, cnfg.va, nullptr, nullptr, 0, 0, -1};
    const uint32_t count = core.va[0x4 / 4] & 0x7ffu;
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        const uint32_t map = core.va[(0x2000 + 4 * i) / 4];
        if (((map >> 8) & 0xfffu) != 0x7fdu) continue;   // SID 7, peripheral 0xfd
        const int owner = (int)(cnfg.va[i] & 7u);
        if (s.apid < 0 || owner == 0) s.apid = (int)i;
    }
    bool ok = s.apid >= 0 && mapRegion(obs, 0xc440000, 0x20ull * (uint32_t)(s.apid + 1)) && mapRegion(chn, 0xc500000, 0x1000ull * (uint32_t)(s.apid + 1));
    if (ok) {
        s.obs = obs.va; s.chn = chn.va;
        if (init) {
            ok = spmiWrite(s, kQcomRepeaterEnable, kQcomRepeaterEnableBit);
            for (const auto &t : kQcomRepeaterTune) ok = ok && spmiWrite(s, t.offset, t.value);
        }
        const uint8_t force = host ? 1u << 6 : 0;   // F_CLK_19P2M_EN, V_CLK_19P2M_EN
        ok = ok && spmiWrite(s, kQcomRepeaterForceEn5, force) && spmiWrite(s, kQcomRepeaterForceVal5, force);
        if (init) {
            IODelay(50);
            uint8_t status = 0;
            ok = ok && spmiRead(s, kQcomRepeaterStatus, &status);
            native_usb_stats[8] = status | (uint32_t)s.apid << 16;
            ok = ok && (status & kQcomRepeaterOk);
        }
    }
    Region *regions[] = {&core, &cnfg, &obs, &chn};
    for (Region *r : regions) if (r->map) r->map->release();
    return ok;
}
void phyInit() {
    for (const auto &w : kQcomEusb2Init) {
        volatile uint32_t *reg = gPhy.va + w.offset / 4;
        *reg = (*reg & ~(uint32_t)w.mask) | w.value;
        barrier();
    }
}
#endif
// dwc3_qcom_select_utmi_clk: no PIPE clock until the QMP PHY runs.
void selectUtmiClock(bool useUtmi = true) {
    volatile uint32_t *cfg = gScratch.va + 0x8 / 4;
    *cfg = *cfg | 1u << 8; barrier(); IOSleep(1);
    const uint32_t utmiBits = 1u | 1u << 3;
    *cfg = (*cfg & ~utmiBits) | (useUtmi ? utmiBits : 0u); barrier(); IOSleep(1);
    *cfg = *cfg & ~(1u << 8); barrier();
}
#ifdef QCOM_DWC3_UKE_USB2
bool restoreUkePipeClock() {
    selectUtmiClock(!gSuperSpeed);
    if (gSuperSpeed && (gScratch.va[0x8 / 4] & 0x109u) != 0) {
        printf("[usb] uke PIPE selection failed; returning to UTMI\n");
        gSuperSpeed = false;
        selectUtmiClock();
    }
    return (gScratch.va[0x8 / 4] & 0x109u) == (gSuperSpeed ? 0u : 9u);
}
#endif
bool vbusOverride(bool on) {
    volatile uint32_t *ss = gScratch.va + 0x30 / 4, *hs = gScratch.va + 0x10 / 4;
    const uint32_t ssBits = 1u << 24, hsBits = 1u << 20 | 1u << 28;
    if (on) {
        if (gSuperSpeed) *ss = *ss | ssBits; else *ss = *ss & ~ssBits;
        *hs = *hs | hsBits;
    } else {
        *ss = *ss & ~ssBits;
        *hs = *hs & ~hsBits;
    }
    barrier();
    const uint32_t hsReadback = *hs, ssReadback = *ss;
    const uint32_t expectedHs = on ? hsBits : 0;
    const uint32_t expectedSs = on && gSuperSpeed ? ssBits : 0;
    native_usb_stats[10] = hsReadback;
    native_usb_stats[52] = ssReadback;
    native_usb_stats[53] = (hsReadback & hsBits) == expectedHs &&
                           (ssReadback & ssBits) == expectedSs;
#ifdef QCOM_DWC3_UKE_USB2
    printf("[usb] uke VBUS override on=%u superspeed=%u hs=%08x ss=%08x verified=%u\n",
           on, gSuperSpeed, hsReadback, ssReadback, native_usb_stats[53]);
#endif
    return native_usb_stats[53] != 0;
}
#ifdef QCOM_DWC3_UKE_USB2
void logSession(const char *phase) {
    printf("[usb flight] session phase=%s general=%08x hs=%08x ss=%08x\n",
           phase, gScratch.va[0x8 / 4], gScratch.va[0x10 / 4], gScratch.va[0x30 / 4]);
}
#endif
#ifndef QCOM_DWC3_UKE_USB2
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
    bool ok = mapRegion(tlmm, 0xf100000, 0x30000) && mapRegion(engine, 0xa80000, 0x4000);
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
    Region tlmm = {};
    if (!mapRegion(tlmm, 0xf100000, 0x10000)) return false;
    const bool reverse = tlmm.va[(11 * 0x1000 + 4) / 4] & 1u;                   // orientation-gpios, tlmm 11, active high
    native_usb_stats[34] = reverse;
    tlmm.map->release();
    return retimerInit(reverse) && qmpInit(reverse);
}
#endif
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
    if (cb[kCbTlbStatus / 4] & 1u) {
        gSmmuTable->complete();
        OSSafeReleaseNULL(gSmmuTable);
        return false;
    }
    cb[kCbFsr / 4] = cb[kCbFsr / 4];
    if (!tablePa || (tablePa & 4095) || cb[kCbTtbr0 / 4] != (uint32_t)tablePa ||
        cb[kCbTtbr0 / 4 + 1] != (uint32_t)(tablePa >> 32) || (cb[kCbMair0 / 4] & 255u) != 255u) {
        gSmmuTable->complete();
        OSSafeReleaseNULL(gSmmuTable);
        return false;
    }
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
    if (!ok) {
        IOLog("qcom-dwc3: SMMU context readback %x %x %x\n", cb[kCbSctlr / 4], gSmmu.va[(0xc00 + 4 * slot) / 4], gSmmu.va[(0x800 + 4 * slot) / 4]);
        gSmmu.va[(0x800 + 4 * slot) / 4] = 0;
        barrier();
        gSmmu.va[(0xc00 + 4 * slot) / 4] = 2u << 16;
        cb[kCbSctlr / 4] = 0;
        barrier();
    }
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
#ifdef QCOM_DWC3_UKE_USB2
    void joinPMtree(IOService *driver) override {
        // Let the platform attach this device to its real power domain. The
        // child may still be queued for attachment when this call returns.
        IOService::joinPMtree(driver);
        printf("[usb] uke PM join child=%s root=%u parent=%u\n", driver->getName(),
               IOService::getPMRootDomain() != nullptr, driver->getParentEntry(gIOPowerPlane) != nullptr);
    }
#endif
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
#ifdef QCOM_DWC3_UKE_USB2
        printf("[usb] uke IRQ registered intid=%u attached=%u active=%u enabled=%u\n",
               kIrq, gLineAttached, gActiveNub == this, enabled);
#endif
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
#ifdef QCOM_DWC3_UKE_USB2
        printf("[usb] uke IRQ enabled intid=%u active=%u\n", kIrq, gActiveNub == this);
#endif
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
static bool gDeviceStackAllowed = true;
bool deviceStackAllowed() {return __atomic_load_n(&gDeviceStackAllowed, __ATOMIC_ACQUIRE);}
void allowDeviceStack(bool allow) {__atomic_store_n(&gDeviceStackAllowed, allow, __ATOMIC_RELEASE);}
class QcomDwc3Xdci : public AppleUSBXDCI {
    OSDeclareDefaultStructors(QcomDwc3Xdci)
#ifdef QCOM_DWC3_UKE_USB2
    static constexpr uint32_t kDeviceConfig = 0x700;
    static constexpr uint32_t kDeviceStatus = 0x70c;
    static constexpr uint32_t kUsb2LpmCapability = 1u << 22;
    static constexpr uint32_t kPowerStateSuspended = 2;
    static constexpr uint32_t kPowerStateOn = 4;
    bool pendingCablePower = false;
    uint32_t cablePowerRetries = 0;
#endif
    uint32_t read(uint32_t offset) {return (*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x450))[offset / 4];}
    void write(uint32_t offset, uint32_t value) {(*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x450))[offset / 4] = value; barrier();}
public:
    bool start(IOService *provider) override {
        uint8_t *self = reinterpret_cast<uint8_t *>(this);
        IOMemoryMap *map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map) return false;
        *reinterpret_cast<IOMemoryMap **>(self + 0x448) = map;
        *reinterpret_cast<uint64_t *>(self + 0x450) = map->getVirtualAddress();
        return AppleUSBXDCI::start(provider);
    }
    // True keeps the family's already selected configuration list (IOUSBDeviceController builds it when this is false): Apple's SoC
    // classes answer from their IOPort transport object, and this class has none. The USB 3 descriptors follow the speed instead.
    bool XDCI_KEEP_CONFIGURATIONS() override {return false;}
    bool XDCI_TRANSPORT_ACTIVE() override {return true;}
    // The power the family asks for at each change, including when configd's description starts the stack: off while the host role has the core.
    uint32_t XDCI_DESIRED_POWER() override {
        const bool allowed = deviceStackAllowed();
        const uint32_t desired = allowed ? AppleUSBXDCI::XDCI_DESIRED_POWER() : 0;
#ifdef QCOM_DWC3_UKE_USB2
        static uint32_t last = ~0u;
        if (desired != last) {
            printf("[usb] uke XDCI desired=%u current=%u allowed=%u\n",
                   desired, static_cast<unsigned>(getPowerState()), allowed);
            last = desired;
        }
#endif
        return desired;
    }
    IOReturn XDCI_POWER_ON() override {
#ifdef QCOM_DWC3_UKE_USB2
        UkeUsbRegistersGuard guard;
        if (!deviceStackAllowed() || gUkeHostCore) return kIOReturnNotReady;
#endif
        native_usb_stats[12] = native_usb_stats[12] + 1;
        if (!vbusOverride(true)) return kIOReturnNotReady;
        native_usb_stats[15] = read(0x70c);
        printf("[usb] uke XDCI power-on count=%u dsts=%08x dctl=%08x\n",
               native_usb_stats[12], native_usb_stats[15], read(0x704));
        return kIOReturnSuccess;
    }
    IOReturn XDCI_POWER_OFF() override {
#ifdef QCOM_DWC3_UKE_USB2
        UkeUsbRegistersGuard guard;
        if (gUkeHostCore) return kIOReturnSuccess;
#endif
        native_usb_stats[13] = native_usb_stats[13] + 1;
        vbusOverride(false);
        printf("[usb] uke XDCI power-off count=%u dsts=%08x dctl=%08x\n",
               native_usb_stats[13], read(0x70c), read(0x704));
        return kIOReturnSuccess;
    }
    // One event buffer and interrupter; the Linux driver has one as well. The scale and the periodic count are read in the reset.
    void XDCI_PARAMETERS() override {
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2628);
        fields[0] = 1; fields[1] = 0; fields[2] = 4; fields[3] = 4;
    }
#if defined(QCOM_DWC3_UKE_USB2) && defined(XDCI_CONNECTION_DONE)
    void XDCI_CONNECTION_DONE() override {
        AppleUSBXDCI::XDCI_CONNECTION_DONE();
        // Uke can receive bus reset and connection-done after a cable replug
        // while the family still holds the power request from the old suspend.
        if (!deviceStackAllowed() || getPowerState() != kPowerStateSuspended ||
            getDesiredPowerState() != kPowerStateSuspended)
            return;
        const uint32_t dsts = read(kDeviceStatus);
        const uint32_t speed = dsts & 7u;
        if (speed != 0 && speed != 1 && speed != 3 && !(gSuperSpeed && speed == 4)) return;
        const IOReturn result = changePowerStateTo(kPowerStateOn);
        printf("[usb] uke reconnect power request=%08x dsts=%08x\n", static_cast<uint32_t>(result), dsts);
    }
#endif
#if defined(QCOM_DWC3_UKE_USB2) && defined(XDCI_UPDATE_REGISTER_FIELD)
    void XDCI_UPDATE_REGISTER_FIELD(uint32_t offset, uint32_t shift, uint32_t mask, uint32_t value) override {
        // The stock stack enables DCFG.LPMCap after controller reset. Uke's
        // OEM snps,usb2-gadget-lpm-disable quirk requires that bit to stay clear.
        if (offset == kDeviceConfig && (mask & kUsb2LpmCapability)) {
            const uint32_t lpmFieldBit = kUsb2LpmCapability >> shift;
            AppleUSBXDCI::XDCI_UPDATE_REGISTER_FIELD(offset, shift, mask, value & ~lpmFieldBit);
            if (value & lpmFieldBit)
                printf("[usb] uke USB2 gadget LPM disabled dcfg=%08x\n", read(kDeviceConfig));
            return;
        }
        AppleUSBXDCI::XDCI_UPDATE_REGISTER_FIELD(offset, shift, mask, value);
    }
#endif
    IOReturn XDCI_CONTROLLER_RESET() override {
#ifdef QCOM_DWC3_UKE_USB2
        // The stock Apple virtual method writes DCTL.CSFTRST. Use the Cliffs block reset with the
        // device stopped, and require a fresh core ID and a halted device after restoring the clock.
        constexpr uint32_t dctlBusy = (1u << 31) | (1u << 30);
        UkeUsbRegistersGuard guard;
        if (!deviceStackAllowed() || gUkeHostCore) return kIOReturnNotReady;
        IOReturn result = kIOReturnNotReady;
        logSession("pre-reset");
        if (!(read(0x704) & dctlBusy) && (read(0x70c) & (1u << 22)) && uke_usb_reset_controller()) {
            logSession("post-reset");
            if (!restoreUkePipeClock()) return result;
            const uint32_t id = read(0x120);
            write(0x110, (read(0x110) & ~(3u << 12)) | 2u << 12);
            // The external block reset can clear QSCRATCH session-valid state.
            // Restore it after reset and leave the SS lane off in USB2-only mode.
            const bool vbusReady = vbusOverride(deviceStackAllowed());
            logSession("restored");
            if (vbusReady && id == native_usb_stats[1] && !(read(0x704) & dctlBusy) &&
                (read(0x70c) & (1u << 22)) && (read(0x110) & (3u << 12)) == (2u << 12))
                result = kIOReturnSuccess;
        }
#else
        const IOReturn result = AppleUSBXDCI::XDCI_CONTROLLER_RESET();
#endif
        native_usb_stats[14] = (uint32_t)result;
        printf("[usb] uke XDCI reset result=%08x dsts=%08x dctl=%08x\n",
               static_cast<uint32_t>(result), read(0x70c), read(0x704));
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
#ifdef QCOM_DWC3_UKE_USB2
        const uint32_t speed = gSuperSpeed ? 4u : 0u; // DWC3 SuperSpeed Gen 1 or high speed
        write(kDeviceConfig, (read(kDeviceConfig) & ~(7u | kUsb2LpmCapability)) | speed);
#else
        if (!gSuperSpeed) write(0x700, read(0x700) & ~7u);   // High speed
#endif
        native_usb_stats[16] = read(0x700); native_usb_stats[17] = read(0x110); native_usb_stats[18] = read(0x200);
        return result;
    }
    // Bandwidth accounting of Apple's fabric: nothing to vote for on this SoC, the SN2 vote covers the peak.
    // The family's power states: 4 starts the bus (the cable-connected path of Apple's SoC classes), 0 takes it off.
    IOReturn cable(bool connected) {
        const IOReturn result = changePowerStateTo(connected ? 4 : 0);
#ifdef QCOM_DWC3_UKE_USB2
        // The family does PMinit/registerPowerDriver only after configd has
        // supplied the configuration. A request before then is rejected,
        // rather than queued by IOService; retain it until that point.
        __atomic_store_n(&pendingCablePower, connected && result == kIOPMNotYetInitialized, __ATOMIC_RELEASE);
#endif
        printf("[usb] uke XDCI power-request connected=%u state=%u result=%08x\n",
               connected, static_cast<unsigned>(getPowerState()), static_cast<uint32_t>(result));
        return result;
    }
#ifdef QCOM_DWC3_UKE_USB2
    void retryCablePower() {
        if (!deviceStackAllowed() || !__atomic_load_n(&pendingCablePower, __ATOMIC_ACQUIRE) || !initialized || !getParentEntry(gIOPowerPlane)) return;
        if (XDCI_DESIRED_POWER() != 4) return;
        if (cablePowerRetries == 5) {
            printf("[usb] uke PM deferred request exhausted parent=%u current=%u\n",
                   getParentEntry(gIOPowerPlane) != nullptr, static_cast<unsigned>(getPowerState()));
            __atomic_store_n(&pendingCablePower, false, __ATOMIC_RELEASE);
            return;
        }
        ++cablePowerRetries;
        native_usb_stats[20] = static_cast<uint32_t>(cable(true));
        printf("[usb] uke PM deferred request=%08x initialized=%u parent=%u current=%u\n",
               native_usb_stats[20], initialized, getParentEntry(gIOPowerPlane) != nullptr,
               static_cast<unsigned>(getPowerState()));
    }
    void logPowerState() {
        printf("[usb] uke PM ready=%u parent=%u pending=%u provider_open=%u\n",
               initialized, getParentEntry(gIOPowerPlane) != nullptr, __atomic_load_n(&pendingCablePower, __ATOMIC_ACQUIRE),
               getProvider()->isOpen(this));
    }
#endif
    uint32_t XDCI_PERIODIC_USAGE() override {return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x58c) != 0;}
    void XDCI_PERIODIC_SET(uint32_t) override {}
};
OSDefineMetaClassAndStructors(QcomDwc3Xdci, AppleUSBXDCI)

// Host mode: Apple's xHCI driver as it is on a PCI controller, with what Linux's dwc3 host glue requires of the core.
class QcomDwc3Xhci : public AppleUSBXHCI {
    OSDeclareDefaultStructors(QcomDwc3Xhci)
#ifdef QCOM_DWC3_UKE_USB2
    unsigned doorbellLogRows = 0;
    struct EventCursor {
        uint64_t base = 0;
        unsigned dequeue = 0;
        unsigned cycle = 1;
        uint32_t pendingControl = 0;
        uint32_t pendingStatus = 0;
    };
    static constexpr unsigned kTraceInterrupters = 4;
    EventCursor eventCursors[kTraceInterrupters] = {};
    unsigned transferLogRows = 0;
    uint8_t endpointSuccessRows[32] = {};
    uint64_t commandRing = 0;
    unsigned secondaryProgress = 0;
    OSSharedPtr<IOTimerEventSource> secondaryTimer;

    static void secondaryTimerAction(OSObject *owner, IOTimerEventSource *timer) {
        auto *host = static_cast<QcomDwc3Xhci *>(owner);
        if (host->isInactive()) return;
        if (gXhciRegs.va && host->getPowerState() == 3) host->serviceSecondaryInterrupts();
        timer->setTimeoutMS(4);
    }

    void serviceSecondaryInterrupts() {
        const auto *regs = gXhciRegs.va;
        const uint32_t runtimeOffset = regs[0x18 / 4] & ~31u;
        const uint32_t count = (regs[1] >> 8) & 0x7ffu;
        if (runtimeOffset < 0x20 || runtimeOffset > 0x8000 - 0x80 || count < 2)
            return;
        const auto *normal = regs + (runtimeOffset + 0x40) / 4;
        const auto *isochronous = regs + (runtimeOffset + 0x60) / 4;
        const bool normalReady = (normal[2] & 0xffffu) != 0;
        const bool isochronousReady = count > 2 && (isochronous[2] & 0xffffu) != 0;
        const uint64_t normalBefore = normalReady ? readUkeRegisterPair(normal + 6) : 0;
        const uint64_t isoBefore = isochronousReady ? readUkeRegisterPair(isochronous + 6) : 0;
        const uint32_t normalIman = normalReady ? normal[0] : 0;
        const uint32_t isoIman = isochronousReady ? isochronous[0] : 0;
        // Only the primary ring has a routed IRQ on Cliffs. Service the
        // other rings on the xHCI work-loop thread, like a native IRQ action.
        if (isochronousReady) AppleUSBXHCI::isochronousInterrupt(nullptr, 0);
        if (normalReady) AppleUSBXHCI::ioInterrupt(nullptr, 0);
        const uint64_t normalAfter = normalReady ? readUkeRegisterPair(normal + 6) : 0;
        const uint64_t isoAfter = isochronousReady ? readUkeRegisterPair(isochronous + 6) : 0;
        if (((normalBefore ^ normalAfter) & ~15ull) || ((isoBefore ^ isoAfter) & ~15ull)) {
            const unsigned progress = ++secondaryProgress;
            if (progress == 1) {
                auto *loop = getWorkLoop();
                printf("[usb otg] xHCI secondary context workloop=%u gated=%u\n",
                       loop && loop->onThread(), loop && loop->inGate());
            }
            if (progress <= 64 || !(progress % 128)) {
                printf("[usb otg] xHCI secondary service count=%u iman=%08x/%08x normal=%llx->%llx iso=%llx->%llx irq=%llu\n",
                       progress, normalIman, isoIman, (unsigned long long)normalBefore, (unsigned long long)normalAfter,
                       (unsigned long long)isoBefore, (unsigned long long)isoAfter,
                       (unsigned long long)armcpu_spi_count(kIrq));
            }
        }
        traceHostInterrupters(runtimeOffset, count);
    }

    void traceCommandRegister(volatile uint32_t *address, uint64_t value) {
        const uint32_t capLength = gXhciRegs.va[0] & 0xffu;
        if ((reinterpret_cast<uintptr_t>(address) & 0xfffu) != capLength + 0x18 ||
            ml_vtophys(reinterpret_cast<vm_offset_t>(address)) != kCore + capLength + 0x18) return;
        const uint64_t base = value & ~63ull;
        if (base) __atomic_store_n(&commandRing, base, __ATOMIC_RELAXED);
    }

    void traceEndpointZero(const volatile uint32_t *op, uint32_t slot) {
        if (slot > (gXhciRegs.va[1] & 0xffu)) return;
        const uint64_t dcbaa = readUkeRegisterPair(op + 0x30 / 4) & ~63ull;
        if (!dcbaa) return;
        uint32_t contextAddress[2];
        if (!readUkeDmaWords(dcbaa + slot * 8, contextAddress)) return;
        const uint64_t context = contextAddress[0] | (uint64_t)contextAddress[1] << 32;
        if (!context || (context & 63u)) return;
        const unsigned stride = (gXhciRegs.va[0x10 / 4] & (1u << 2)) ? 64 : 32;
        uint32_t ep[5];
        if (!readUkeDmaWords(context + stride, ep)) return;
        printf("[usb otg] xHCI EP0 context slot=%u state=%u type=%u mps=%u cerr=%u dcs=%u avg=%u\n",
               slot, ep[0] & 7u, (ep[1] >> 3) & 7u, ep[1] >> 16, (ep[1] >> 1) & 3u,
               ep[2] & 1u, ep[4] & 0xffffu);
        const uint64_t dequeue = (ep[2] & ~15u) | (uint64_t)ep[3] << 32;
        const unsigned pageTrbs = (4096 - (dequeue & 0xfffu)) / 16;
        for (unsigned i = 0; i < 6 && i < pageTrbs; ++i) {
            uint32_t trb[4];
            if (!readUkeDmaWords(dequeue + 16ull * i, trb)) break;
            const uint32_t type = (trb[3] >> 10) & 0x3fu;
            printf("[usb otg] xHCI EP0 TRB slot=%u part=%u type=%u cycle=%u flags=%04x len=%u intr=%u\n",
                   slot, i, type, trb[3] & 1u, trb[3] & 0x3ffu,
                   trb[2] & 0x1ffffu, trb[2] >> 22);
            if (type == 2 && (trb[3] & (1u << 6))) {
                printf("[usb otg] xHCI EP0 setup slot=%u bmRequestType=%02x request=%02x value=%04x index=%04x length=%u\n",
                       slot, trb[0] & 255u, (trb[0] >> 8) & 255u, trb[0] >> 16,
                       trb[1] & 0xffffu, trb[1] >> 16);
            }
            if (!type || type == 6) break; // End of current segment.
        }
    }
#endif
public:
#ifdef QCOM_DWC3_UKE_USB2
    bool start(IOService *provider) override {
        if (!AppleUSBXHCI::start(provider)) return false;
        auto *loop = getWorkLoop();
        secondaryTimer = OSSharedPtr<IOTimerEventSource>(IOTimerEventSource::timerEventSource(
            kIOTimerEventSourceOptionsPriorityWorkLoop, this, secondaryTimerAction), OSNoRetain);
        if (!loop || !secondaryTimer || loop->addEventSource(secondaryTimer.get()) != kIOReturnSuccess) {
            printf("[usb otg] xHCI secondary timer attach failed\n");
            secondaryTimer.reset();
            AppleUSBXHCI::stop(provider);
            return false;
        }
        secondaryTimer->setTimeoutMS(4);
        return true;
    }

    void stop(IOService *provider) override {
        if (secondaryTimer) {
            secondaryTimer->disable();
            secondaryTimer->cancelTimeout();
            if (auto *loop = secondaryTimer->getWorkLoop()) loop->removeEventSource(secondaryTimer.get());
            secondaryTimer.reset();
        }
        AppleUSBXHCI::stop(provider);
    }
#endif
    // Once the ports exist the family leaves the controller's power to them, and an idle port lets it fall to off: a halted controller reports
    // no attach, and what wakes it on Apple's hardware is the Type-C transport's cable report, which this class does not have. The role follows
    // the port manager, so this controller exists only with a peripheral on the cable: kPowerStateOn for as long as it exists.
    uint32_t XHCI_DESIRED_POWER() override {return 3;}
    // Access the DWC3 xHCI registers as two 32-bit words.
    uint64_t XHCI_READ64(void *base, uint32_t offset) override {
        volatile uint32_t *at = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset);
        const uint64_t low = at[0];
        return low | (uint64_t)at[1] << 32;
    }
    void XHCI_WRITE64(void *base, uint32_t offset, uint64_t value) override {
        volatile uint32_t *at = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset);
#ifdef QCOM_DWC3_UKE_USB2
        // The stock Cliffs xHCI driver uses lo_hi_writeq.
        at[0] = (uint32_t)value;
        barrier();
        at[1] = (uint32_t)(value >> 32);
#else
        // SM8550 follows write-64-hi-lo-quirk.
        at[1] = (uint32_t)(value >> 32);
        barrier();
        at[0] = (uint32_t)value;
#endif
        barrier();
#ifdef QCOM_DWC3_UKE_USB2
        if (gXhciRegs.va) traceCommandRegister(at, value);
#endif
    }
#ifdef QCOM_DWC3_UKE_USB2
    void traceTransferEvents(unsigned index, const volatile uint32_t *ir) {
        constexpr unsigned budget = 256;
        if (transferLogRows >= budget || (ir[2] & 0xffffu) != 1) return;
        const uint64_t erst = readUkeRegisterPair(ir + 4) & ~63ull;
        const uint64_t erdpRaw = readUkeRegisterPair(ir + 6);
        const uint64_t erdp = erdpRaw & ~15ull;
        asm volatile("dmb oshld" ::: "memory");
        uint32_t segment[4];
        if (!readUkeDmaWords(erst, segment)) return;
        const uint64_t base = segment[0] | (uint64_t)segment[1] << 32;
        const unsigned length = segment[2] & 0xffffu;
        if (!base || (base & 63u) || segment[3] || length < 16 || length > 4096 ||
            erdp < base || erdp - base >= 16ull * length || ((erdp - base) & 15u)) return;
        const unsigned current = static_cast<unsigned>((erdp - base) / 16);
        auto &cursor = eventCursors[index];
        if (base != cursor.base || cursor.dequeue >= length) {
            cursor = {base, 0, 1, 0, 0};
            printf("[usb otg] xHCI event segment intr=%u trbs=%u iman=%08x imod=%08x\n",
                   index, length, ir[0], ir[1]);
        }
        // A controller recovery can rewind the ring. Restart the observation
        // budget at its current dequeue rather than walking old entries.
        const unsigned pending = (current + length - cursor.dequeue) % length;
        if (pending > 64) {
            printf("[usb otg] xHCI event discontinuity intr=%u prior=%u current=%u\n",
                   index, cursor.dequeue, current);
            cursor.cycle ^= current < cursor.dequeue;
            cursor.dequeue = current;
            ++transferLogRows;
            return;
        }
        while (cursor.dequeue != current && transferLogRows < budget) {
            uint32_t event[4];
            const unsigned entry = cursor.dequeue;
            const unsigned cycle = cursor.cycle;
            const uint64_t at = base + 16ull * entry;
            cursor.dequeue = (cursor.dequeue + 1) % length;
            if (!cursor.dequeue) cursor.cycle ^= 1;
            if (!readUkeDmaWords(at, event) || (event[3] & 1u) != cycle) continue;
            const unsigned type = (event[3] >> 10) & 63u;
            const unsigned endpoint = (event[3] >> 16) & 31u;
            if (type == 39) continue;
            if (type == 32 && endpoint != 1 && (event[2] >> 24) == 1) {
                if (endpointSuccessRows[endpoint] >= 4) continue;
                ++endpointSuccessRows[endpoint];
            }
            ++transferLogRows;
            printf("[usb otg] xHCI completion intr=%u entry=%u type=%u slot=%u endpoint=%u cc=%u length=%u ed=%u\n",
                   index, entry, type, event[3] >> 24, endpoint, event[2] >> 24,
                   event[2] & 0xffffffu, (event[3] >> 2) & 1u);
            if (type == 33 || (type == 32 && endpoint == 1 && !(event[3] & 4u))) {
                const uint64_t pointer = (event[0] & ~15u) | (uint64_t)event[1] << 32;
                uint32_t trb[4];
                if (readUkeDmaWords(pointer, trb))
                    printf("[usb otg] xHCI completion TRB intr=%u entry=%u type=%u cycle=%u flags=%04x len=%u intr_target=%u\n",
                           index, entry, (trb[3] >> 10) & 63u, trb[3] & 1u,
                           trb[3] & 0x3ffu, trb[2] & 0x1ffffu, trb[2] >> 22);
            }
        }
        uint32_t next[4];
        if (transferLogRows >= budget || !readUkeDmaWords(erdp, next) ||
            (next[3] & 1u) != cursor.cycle) {
            cursor.pendingControl = 0;
            cursor.pendingStatus = 0;
            return;
        }
        if (next[3] == cursor.pendingControl && next[2] == cursor.pendingStatus && !pending) return;
        cursor.pendingControl = next[3];
        cursor.pendingStatus = next[2];
        ++transferLogRows;
        printf("[usb otg] xHCI pending event intr=%u entry=%u control=%08x status=%08x iman=%08x ehb=%u\n",
               index, current, next[3], next[2], ir[0], (unsigned)((erdpRaw >> 3) & 1u));
    }

    void traceHostInterrupters(uint32_t runtimeOffset, uint32_t count) {
        for (unsigned index = 0; index < count && index < kTraceInterrupters; ++index) {
            const uint32_t offset = runtimeOffset + (index + 1) * 0x20;
            if (offset > 0x8000 - 0x20) return;
            traceTransferEvents(index, gXhciRegs.va + offset / 4);
        }
    }

    IOReturn ringDoorbell(uint32_t slot, uint32_t target) override {
        // Mirror the Linux writel ordering at the DWC3 boundary, then flush
        // the posted doorbell with a read from the same MMIO register.
        asm volatile("dmb oshst" ::: "memory");
        const IOReturn result = AppleUSBXHCI::ringDoorbell(slot, target);
        if (result == kIOReturnSuccess && gXhciRegs.va) {
            const uint32_t base = gXhciRegs.va[0x14 / 4] & ~3u;
            const uint32_t slots = gXhciRegs.va[1] & 0xffu;
            if (slot <= slots && base >= 0x20 && base <= 0x7ffc - slot * 4)
                (void)gXhciRegs.va[base / 4 + slot];
        }
        // Sample the command ring and endpoint zero during enumeration.
        if ((slot == 0 || (target & 0xffu) == 1) &&
            __atomic_load_n(&doorbellLogRows, __ATOMIC_RELAXED) < 48 &&
            __atomic_fetch_add(&doorbellLogRows, 1, __ATOMIC_RELAXED) < 48) {
            uint32_t portsc = UINT32_MAX;
            if (!slot) {
                const auto base = __atomic_load_n(&commandRing, __ATOMIC_RELAXED);
                printf("[usb otg] xHCI command ring base=%llx\n", (unsigned long long)base);
            }
            if (slot && result == kIOReturnSuccess && gXhciRegs.va) {
                const uint32_t cap = gXhciRegs.va[0] & 0xffu;
                if (cap >= 0x20 && !(cap & 3u)) {
                    const auto *op = gXhciRegs.va + cap / 4;
                    portsc = op[0x400 / 4];
                    traceEndpointZero(op, slot);
                }
            }
            printf("[usb otg] xHCI doorbell slot=%u target=%08x result=%08x usb2_portsc=%08x\n",
                   slot, target, static_cast<uint32_t>(result), portsc);
        }
        return result;
    }
#endif
};
OSDefineMetaClassAndStructors(QcomDwc3Xhci, AppleUSBXHCI)

namespace {
bool bringUp() {
    stage(1);
#ifdef QCOM_DWC3_UKE_USB2
    if (uke_usb_otg_enabled()) {
        gUkeUsbRegisters = IOLockAlloc();
        if (!gUkeUsbRegisters) {stage(-1); return false;}
    }
    if (!mapRegion(gScratch, kQscratch & ~0xfffull, 0x1000)) {stage(-1); return false;}
    gScratch.va += (kQscratch & 0xfff) / 4;
    const int failure = uke_usb2_hardware_init();
    if (failure) {stage(failure); return false;}
    stage(5);
    if (!smmuIdentity()) {stage(-5); return false;}
    unsigned usb3 = 0;
    if (PE_parse_boot_argn("uke-usb3", &usb3, sizeof usb3) && usb3 == 1) {
        gSuperSpeed = uke_usb3_hardware_init();
        printf("[usb] uke USB3 PHY enabled=%u\n", gSuperSpeed);
    }
    if (!restoreUkePipeClock()) {stage(-8); return false;}
#else
    if (!mapRegion(gGcc, kGcc, 0x1f0000) || !mapRegion(gTcsrMap, kTcsr, 0x1000) || !mapRegion(gPhy, kPhy, 0x1000) ||
        !mapRegion(gScratch, kQscratch & ~0xfffull, 0x1000)) {stage(-1); return false;}
    gScratch.va += (kQscratch & 0xfff) / 4;
    stage(2);
    if (!powerRails()) {stage(-2); return false;}
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
#endif
    stage(8);
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000)) {stage(-8); return false;}
    const uint32_t id = window.va[0x120 / 4];
    native_usb_stats[1] = id;
    window.map->release();
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
uint32_t gWantedGeneration, gAppliedGeneration;
int gWantedLane = -1, gAppliedLane = -1;
#ifdef QCOM_DWC3_UKE_USB2
// The PMIC work loop can receive a connector report before USB publishes.
uint64_t gUkeReportedRole;
int gUkeProgrammedLane = -1;
bool gUkeProgrammedDp;
int gUkeDpLane = -1;
uint32_t gUkeDpGeneration, gUkeDpProbeRequested, gUkeDpProbeCompleted, gUkeDpAttention;
bool gUkeDpHpd;
unsigned gUkeHostSamples;
uint32_t gUkeLastHostPort[2];
unsigned gUkeHostPortRows;
unsigned gUkeHostPolls;
#endif
bool gForceHost;
Region gGlobals;
QcomDwc3Nub *gDeviceNub, *gHostNub;
QcomDwc3Xhci *gHost;
bool wantedDpRoute(int role, int lane, uint32_t generation) {
#ifdef QCOM_DWC3_UKE_USB2
    return role == kRoleHost && lane >= 0 && lane == gUkeDpLane && generation == gUkeDpGeneration;
#else
    (void)role; (void)lane; (void)generation;
    return false;
#endif
}
#ifdef QCOM_DWC3_UKE_USB2
struct UkeDpConnection {
    int lane;
    uint32_t generation, probe, attention;
};

UkeDpActivity waitUkeDpVideo(void *context) {
    auto &connection = *static_cast<UkeDpConnection *>(context);
    auto stillConnected = [&] {
        return gRoleWanted == kRoleHost && gWantedGeneration == connection.generation &&
               gWantedLane == connection.lane && gUkeDpProbeRequested == connection.probe &&
               wantedDpRoute(gRoleWanted, gWantedLane, gWantedGeneration) && gUkeDpHpd;
    };
    IOLockLock(gRoleLock);
    if (stillConnected() && connection.attention == gUkeDpAttention) {
        uint64_t deadline;
        clock_interval_to_deadline(250, kMillisecondScale, &deadline);
        IOLockSleepDeadline(gRoleLock, &gRoleWanted, deadline, THREAD_UNINT);
    }
    UkeDpActivity activity = UkeDpActivity::Disconnect;
    if (stillConnected()) {
        activity = connection.attention == gUkeDpAttention ? UkeDpActivity::Hold : UkeDpActivity::Attention;
        connection.attention = gUkeDpAttention;
    }
    IOLockUnlock(gRoleLock);
    return activity;
}
#endif
void wantRole(int role, uint32_t generation = 0) {
    IOLock *lock = __atomic_load_n(&gRoleLock, __ATOMIC_ACQUIRE);
    if (!lock) return;
    IOLockLock(lock);
#ifdef QCOM_DWC3_UKE_USB2
    const uint64_t report = __atomic_load_n(&gUkeReportedRole, __ATOMIC_ACQUIRE);
    if (report) {
        role = static_cast<int>(report & 0xffu) - 1;
        gWantedLane = static_cast<int>((report >> 8) & 3u) - 1;
        generation = report >> 32;
    }
#endif
    gRoleWanted = role;
    gWantedGeneration = generation;
    IOLockWakeup(lock, &gRoleWanted, false);
    IOLockUnlock(lock);
}
// What is known now, by ucsi_partner_change: the port manager when it answered, which tells a USB host from a charger and a peripheral; the
// charger firmware's online value until then, or if UCSI does not come up.
void cableChanged(int lane = -1) {
    int role = gVbusOn > 0 ? kRoleDevice : kRoleNone;
    if (gUcsi.status_valid) {
        role = kRoleNone;
        if (gUcsi.connected && gUcsi.usb) {
            if (gUcsi.partner == kUcsiPartnerDfp) role = kRoleDevice;
            else if (gUcsi.partner == kUcsiPartnerUfp || gUcsi.partner == kUcsiPartnerCableUfp) role = kRoleHost;
        }
    }
#ifdef QCOM_DWC3_UKE_USB2
    // USB input voltage alone never authorizes entering xHCI mode.
    if (!gUcsi.status_valid) role = kRoleDevice;
    const uint32_t generation = gUcsi.connection_generation;
    __atomic_store_n(&gUkeReportedRole, (static_cast<uint64_t>(generation) << 32) |
                     (static_cast<uint32_t>(lane + 1) << 8) | (role + 1u), __ATOMIC_RELEASE);
    wantRole(role, generation);
#else
    (void)lane;
    wantRole(gForceHost ? kRoleHost : role);
#endif
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
bool leaveRole(int role) {
    if (role == kRoleDevice) {
        IOService::getResourceService()->removeProperty("AppleUSBCableType");
        // The family stops the bus through XDCI_POWER_OFF; with no description from configd yet the bus was never on and it does not come.
#ifndef QCOM_DWC3_UKE_USB2
        const uint32_t before = native_usb_stats[13];
#endif
        native_usb_stats[20] = (uint32_t)gController->cable(false);
#ifdef QCOM_DWC3_UKE_USB2
        // Check both the family's power state and the hardware halt bit
        // before moving interrupts or resetting the common controller.
        for (unsigned i = 0; i < 200; ++i) {
            if (gController->getPowerState() == 0 || native_usb_stats[20] == kIOPMNotYetInitialized) {
                UkeUsbRegistersGuard guard;
                if (!(gGlobals.va[0x704 / 4] & (3u << 30)) &&
                    (gGlobals.va[0x70c / 4] & (1u << 22))) return true;
            }
            IOSleep(10);
        }
        printf("[usb otg] device failed to halt power=%u dctl=%08x dsts=%08x\n",
               (unsigned)gController->getPowerState(), gGlobals.va[0x704 / 4], gGlobals.va[0x70c / 4]);
        return false;
#else
        for (unsigned i = 0; i < 200 && native_usb_stats[13] == before; ++i) IOSleep(10);
#endif
    } else if (role == kRoleHost && gHostNub) {
        // A removed PCI controller's path: the family stops its devices and the controller.
#ifdef QCOM_DWC3_UKE_USB2
        // The family tears down its endpoints before we touch USBCMD.
        if (!gHostNub->isInactive() && !gHostNub->terminate(kIOServiceSynchronous)) return false;
        OSSafeReleaseNULL(gHost);
        const unsigned capLength = gXhciRegs.va[0] & 0xffu;
        if (capLength < 0x20 || (capLength & 3u)) return false;
        volatile uint32_t *op = gXhciRegs.va + capLength / 4;
        op[0] &= ~1u;
        barrier();
        unsigned retries = 200;
        while (!(op[1] & 1u) && retries--) IOSleep(1);
        if (!(op[1] & 1u)) {
            printf("[usb otg] xHCI failed to halt cmd=%08x sts=%08x\n", op[0], op[1]);
            return false;
        }
#else
        gHostNub->terminate(kIOServiceSynchronous);
#endif
        gHostNub->release();
        gHostNub = nullptr; gHost = nullptr;
        QcomDwc3Nub::route(nullptr);
    }
    return true;
}
// Quiesce the old role before resetting and changing the port capability.
#ifdef QCOM_DWC3_UKE_USB2
void applyUkeHostTiming(volatile uint32_t *regs, bool host) {
    // Keep the eUSB2 PHY awake while the Apple host stack owns the bus,
    // as in sheng's setCoreRole and the Cliffs runtime-resume path.
    constexpr uint32_t suspendPhy = 1u << 6, enableSleep = 1u << 8;
    constexpr uint32_t l1SuspendThreshold = 1u << 8, sparseControl = 1u << 17;
    constexpr unsigned guctl1 = 0x11c / 4, guctl3 = 0x60c / 4;
    regs[0x200 / 4] &= ~(suspendPhy | enableSleep);
    regs[guctl1] = (regs[guctl1] & ~l1SuspendThreshold) | (host ? l1SuspendThreshold : 0u);
    regs[0x12c / 4] = (regs[0x12c / 4] & ~sparseControl) | (host ? sparseControl : 0u);
    // Cliffs enables PIPE SUSPHY in host mode when the QMP/PIPE clock is on.
    // The stock host dump shows this bit set during USB2 enumeration.
    if (host && gSuperSpeed) regs[0x2c0 / 4] |= 1u << 17;
    if (host && (regs[0x120 / 4] >> 16) == 0x3331) {
        constexpr uint32_t revision170a = 0x3137302a;
        constexpr uint32_t interPacketGapMask = 7u << 21, usb20RetryDisable = 1u << 16;
        const uint32_t revision = regs[0x1a0 / 4];
        const bool applyHostErrata = revision >= revision170a && revision != UINT32_MAX;
        if (applyHostErrata) {
            // Cliffs dwc3_otg_start_host applies EL_23 and STAR 9001346572.
            regs[guctl1] = (regs[guctl1] & ~interPacketGapMask) | (1u << 21);
            regs[guctl3] |= usb20RetryDisable;
            barrier();
        }
        printf("[usb otg] DWC31 host revision=%08x type=%08x errata=%u guctl1=%08x guctl3=%08x\n",
               revision, regs[0x1a4 / 4], applyHostErrata, regs[guctl1], regs[guctl3]);
    }
    barrier();
    printf("[usb otg] host timing role=%s usb2phy=%08x guctl1=%08x guctl=%08x pipe=%08x\n",
           host ? "host" : "device", regs[0x200 / 4], regs[guctl1], regs[0x12c / 4], regs[0x2c0 / 4]);
}
#endif
#ifdef QCOM_DWC3_UKE_USB2
void programUkeQmp(int lane, bool dp) {
    // The old Apple role has stopped and gUkeUsbRegisters is held.
    selectUtmiClock();
    if (gUkeProgrammedDp && !dp && !uke_usb_dp_route_aux(-1))
        printf("[dp route] WCD AUX release failed\n");
    gUkeProgrammedDp = false;
    if (dp) {
        gUkeProgrammedDp = uke_usb3_set_orientation(lane, true) && uke_usb_dp_route_aux(lane);
        if (!gUkeProgrammedDp) {
            const bool isolated = uke_usb_dp_route_aux(-1);
            printf("[dp route] combined route failed lane=%d isolated=%u; restoring USB\n", lane, isolated);
        }
    }
    gSuperSpeed = gUkeProgrammedDp || uke_usb3_set_orientation(lane);
    if (gSuperSpeed) gUkeProgrammedLane = lane;
    else printf("[usb otg] QMP lane=%d failed, using UTMI\n", lane);
}
#endif
bool setCoreRole(int role, int lane, bool dp) {
#ifdef QCOM_DWC3_UKE_USB2
    const bool laneChanged = gSuperSpeed && (gUkeProgrammedLane != lane || gUkeProgrammedDp != dp);
    const bool roleChanged = gCoreRole != role;
    if (!roleChanged && !laneChanged) return true;
    QcomDwc3Nub::route(nullptr);
    UkeUsbRegistersGuard guard;
    gCoreRole = kRoleNone;
    gUkeHostCore = true; // Block device MMIO callbacks until the mode is verified.
    if (!uke_usb_set_repeater_host(role == kRoleHost, roleChanged) || !uke_usb_reset_controller()) return false;
    if (laneChanged) {
        // The previous stack is stopped. Select the new lane before the PCS
        // restarts, then switch the controller back to the PHY's PIPE clock.
        programUkeQmp(lane, dp);
    }
    if (!restoreUkePipeClock()) return false;
    if (gGlobals.va[0x120 / 4] != native_usb_stats[1]) return false;
    const uint32_t prtcap = (role == kRoleHost ? 1u : 2u) << 12;
    gGlobals.va[0x110 / 4] = (gGlobals.va[0x110 / 4] & ~(3u << 12)) | prtcap;
    gGlobals.va[0x2c0 / 4] &= ~(1u << 17);
    applyUkeHostTiming(gGlobals.va, role == kRoleHost);
    barrier();
    const bool modeReady = (gGlobals.va[0x110 / 4] & (3u << 12)) == prtcap;
    const bool sessionCleared = vbusOverride(false);
    printf("[usb otg] core role=%d gctl=%08x ready=%u session_cleared=%u\n",
           role, gGlobals.va[0x110 / 4], modeReady, sessionCleared);
    if (!modeReady || !sessionCleared) return false;
    gCoreRole = role;
    gUkeHostCore = role == kRoleHost;
    return true;
#else
    (void)lane;
    (void)dp;
    if (gCoreRole == role) return true;
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
    allowDeviceStack(role == kRoleDevice);
    repeater(false, role == kRoleHost);
    return true;
#endif
}
// qmp_combo_typec_switch_set and ps5169: the lanes follow the plug. The GPIO means something once a cable is in.
void followOrientation() {
#ifdef QCOM_DWC3_UKE_USB2
    return;
#else
    Region tlmm = {};
    if (!mapRegion(tlmm, 0xf100000, 0x10000)) return;
    const uint32_t reverse = tlmm.va[(11 * 0x1000 + 4) / 4] & 1u;
    tlmm.map->release();
    if (gSuperSpeed && reverse != native_usb_stats[34]) {
        gSuperSpeed = superSpeedInit();
        if (!gSuperSpeed) selectUtmiClock();
    }
#endif
}
bool enterHost() {
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
                else {
                    gHost = host;
#ifdef QCOM_DWC3_UKE_USB2
                    gHost->retain();
#endif
                }
            }
            OSSafeReleaseNULL(host);
        }
    }
    native_usb_stats[46] = (native_usb_stats[46] & 0xffff0000u) | (done ? 1u : 2u);
    OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(range); OSSafeReleaseNULL(zero);
    if (done) gHostNub = nub;
    else if (nub) {QcomDwc3Nub::route(nullptr); nub->terminate(kIOServiceSynchronous); nub->release();}
#ifdef QCOM_DWC3_UKE_USB2
    gUkeHostSamples = 0;
    gUkeHostPortRows = 0;
    gUkeHostPolls = 0;
    gUkeLastHostPort[0] = gUkeLastHostPort[1] = ~0u;
    printf("[usb otg] xHCI start=%u\n", done);
#endif
    return done;
}
bool enterRole(int role, int lane, bool dp) {
    if (role == kRoleNone) {
#ifdef QCOM_DWC3_UKE_USB2
        if (gUkeProgrammedDp) {
            UkeUsbRegistersGuard guard;
            programUkeQmp(gUkeProgrammedLane, false);
            return restoreUkePipeClock();
        }
#endif
        return true;
    }
    followOrientation();
    if (!setCoreRole(role, lane, dp)) return false;
    if (role == kRoleHost) {
#ifdef QCOM_DWC3_UKE_USB2
        // The Type-C controller already negotiated the port. Keep PD/VBUS
        // control with it, as in the OEM board without a host vbus-supply.
        { UkeUsbRegistersGuard guard; if (!vbusOverride(false)) return false; }
#else
        vbusOverride(false);
#endif
        return enterHost();
    } else {
        allowDeviceStack(true);
        QcomDwc3Nub::route(gDeviceNub);
        // A USB host on the cable: lockdownd's launch event, then the family goes on the bus when configd's description arrives.
        IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
        native_usb_stats[20] = (uint32_t)gController->cable(true);
    }
    return true;
}
#ifdef QCOM_DWC3_UKE_USB2
void traceUkeHostPorts() {
    if (!gHost || gCoreRole != kRoleHost || gUkeHostPortRows >= 64) return;
    const uint32_t cap = gXhciRegs.va[0] & 0xffu;
    if (cap < 0x20 || (cap & 3u)) return;
    const volatile uint32_t *op = gXhciRegs.va + cap / 4;
    // Watch only connection, enable, reset and link-state bits. Leave
    // PORTSC and its write-one-to-clear bits owned by AppleUSBXHCI.
    constexpr uint32_t changes = (1u << 21) | (1u << 19) | (1u << 17) | 0x1f3u;
    const uint32_t portCount = gXhciRegs.va[1] >> 24;
    for (unsigned port = 0; port < portCount && port < 2 && gUkeHostPortRows < 64; ++port) {
        const uint32_t raw = op[(0x400 + port * 0x10) / 4];
        if ((raw & changes) == (gUkeLastHostPort[port] & changes)) continue;
        gUkeLastHostPort[port] = raw;
        ++gUkeHostPortRows;
        printf("[usb otg] xHCI port trace id=%u ccs=%u ped=%u reset=%u prc=%u pls=%u raw=%08x\n",
               port + 1, raw & 1u, (raw >> 1) & 1u, (raw >> 4) & 1u,
               (raw >> 21) & 1u, (raw >> 5) & 15u, raw);
    }
}

void logUkeHostPortObjects() {
    OSIterator *children = gHost->getChildIterator(gIOServicePlane);
    unsigned count = 0;
    while (children && count < 8) {
        auto *entry = OSDynamicCast(IOService, children->getNextObject());
        if (!entry) break;
        auto *port = OSDynamicCast(OSNumber, entry->getProperty("usb-port-number"));
        const char *name = entry->getMetaClass()->getClassName();
        printf("[usb otg] xHCI child class=%s port=%u power=%u\n", name ? name : "?",
               port ? port->unsigned32BitValue() : 0u, static_cast<unsigned>(entry->getPowerState()));
        ++count;
    }
    printf("[usb otg] xHCI controller_power=%u children=%u hcsparams1=%08x hccparams1=%08x\n",
           static_cast<unsigned>(gHost->getPowerState()), count, gXhciRegs.va[1], gXhciRegs.va[0x10 / 4]);
    OSSafeReleaseNULL(children);
}
#endif
void hostStats() {
    if (gCoreRole != kRoleHost || !gHost) return;
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
#ifdef QCOM_DWC3_UKE_USB2
    const uint32_t rtsoff = gXhciRegs.va[0x18 / 4] & ~31u;
    // Interrupter 0 starts at runtime base + 0x20. Leave hardware-owned
    // W1C status and dequeue registers untouched.
    const unsigned sample = gUkeHostSamples++;
    if (sample == 1 || sample == 3 || sample == 7) logUkeHostPortObjects();
    if ((sample < 12 || !(sample % 5)) && rtsoff >= 0x20 && rtsoff <= 0x7fc0) {
        const volatile uint32_t *ir = gXhciRegs.va + (rtsoff + 0x20) / 4;
        const volatile uint32_t *cb = gSmmu.va + (gSmmuPages + gSmmuBank) * 1024;
        printf("[usb otg] xHCI event irq=%llu iman=%08x erstsz=%08x erst=%08x:%08x erdp=%08x:%08x\n",
               (unsigned long long)armcpu_spi_count(kIrq), ir[0], ir[2], ir[5], ir[4], ir[7], ir[6]);
        printf("[usb otg] xHCI rings crcr=%08x:%08x dcbaa=%08x:%08x config=%08x smmu_fsr=%08x\n",
               op[7], op[6], op[13], op[12], op[14], cb[0x58 / 4]);
        printf("[usb otg] DMA snapshot gfsr=%08x fsynr=%08x far=%08x:%08x dwc_gsts=%08x buserr=%08x:%08x\n",
               gSmmu.va[0x48 / 4], cb[0x68 / 4], cb[0x64 / 4], cb[0x60 / 4],
               gGlobals.va[0x118 / 4], gGlobals.va[0x134 / 4], gGlobals.va[0x130 / 4]);
    }
    uint32_t hidCount = 0;
    auto *hidMatch = IOService::serviceMatching("IOUSBHostHIDDevice");
    auto *hid = hidMatch ? IOService::getMatchingServices(hidMatch) : nullptr;
    while (hid && hid->getNextObject()) ++hidCount;
    OSSafeReleaseNULL(hid);
    OSSafeReleaseNULL(hidMatch);
    static uint32_t lastCount = ~0u, lastHid = ~0u, lastPort1 = ~0u, lastPort2 = ~0u;
    if (sample == 0 || count != lastCount || hidCount != lastHid ||
        native_usb_stats[49] != lastPort1 || native_usb_stats[50] != lastPort2) {
        lastCount = count;
        lastHid = hidCount;
        lastPort1 = native_usb_stats[49];
        lastPort2 = native_usb_stats[50];
        printf("[usb otg] xHCI cmd=%08x sts=%08x port1=%08x port2=%08x devices=%u hid=%u vidpid=%08x\n",
               native_usb_stats[47], native_usb_stats[48], lastPort1, lastPort2, count, hidCount, native_usb_stats[53]);
    }
#endif
    native_usb_stats[52] = count;
    OSSafeReleaseNULL(devices); OSSafeReleaseNULL(match);
}
void roleThread(void *, wait_result_t) {
    int failedRole = -1;
    int failedLane = -1;
    uint32_t failedGeneration = 0;
    bool dpApplied = false, failedDp = false;
    for (;;) {
        IOLockLock(gRoleLock);
        bool dp;
        for (;;) {
            dp = wantedDpRoute(gRoleWanted, gWantedLane, gWantedGeneration);
            const bool alreadyApplied = gRoleWanted == gRoleApplied && gWantedGeneration == gAppliedGeneration &&
                                        gWantedLane == gAppliedLane && dp == dpApplied;
            const bool alreadyFailed = gRoleWanted == failedRole && gWantedGeneration == failedGeneration &&
                                       gWantedLane == failedLane && dp == failedDp;
            if (!alreadyApplied && !alreadyFailed) break;
#ifdef QCOM_DWC3_UKE_USB2
            if (alreadyApplied && dp && gUkeProgrammedDp && gUkeDpHpd &&
                gUkeDpProbeCompleted != gUkeDpProbeRequested) {
                UkeDpConnection connection{gUkeDpLane, gUkeDpGeneration, gUkeDpProbeRequested, gUkeDpAttention};
                gUkeDpProbeCompleted = gUkeDpProbeRequested;
                IOLockUnlock(gRoleLock);
                uke_usb_dp_read_dpcd(connection.lane, waitUkeDpVideo, &connection);
                IOLockLock(gRoleLock);
                continue;
            }
#endif
            if (gRoleApplied != kRoleHost) IOLockSleep(gRoleLock, &gRoleWanted, THREAD_UNINT);
            else {
                uint64_t deadline;
#ifdef QCOM_DWC3_UKE_USB2
                clock_interval_to_deadline(4, kMillisecondScale, &deadline);
#else
                clock_interval_to_deadline(1, kSecondScale, &deadline);
#endif
                IOLockSleepDeadline(gRoleLock, &gRoleWanted, deadline, THREAD_UNINT);
                IOLockUnlock(gRoleLock);
#ifdef QCOM_DWC3_UKE_USB2
                traceUkeHostPorts();
                if (++gUkeHostPolls % 250 == 0) hostStats();
#else
                hostStats();
#endif
                IOLockLock(gRoleLock);
            }
        }
        const int role = gRoleWanted;
        const int lane = gWantedLane;
        const uint32_t generation = gWantedGeneration;
        IOLockUnlock(gRoleLock);
#ifdef QCOM_DWC3_UKE_USB2
        allowDeviceStack(false);
        printf("[usb otg] switch from=%d to=%d lane=%d generation=%u\n", gRoleApplied, role, lane, generation);
#else
        if (role == kRoleHost) allowDeviceStack(false);
#endif
        const bool stopped = leaveRole(gRoleApplied);
        if (stopped) gRoleApplied = kRoleNone;
        // A reconnect can arrive while IOService stops the old stack.
        IOLockLock(gRoleLock);
        const bool current = gRoleWanted == role && gWantedGeneration == generation && gWantedLane == lane &&
                             dp == wantedDpRoute(role, lane, generation);
        IOLockUnlock(gRoleLock);
        if (stopped && !current) {
            gAppliedGeneration = generation;
            continue;
        }
        if (!stopped || !enterRole(role, lane, dp)) {
            failedRole = role;
            failedLane = lane;
            failedGeneration = generation;
            failedDp = dp;
#ifdef QCOM_DWC3_UKE_USB2
            printf("[usb otg] switch failed target=%d stopped=%u generation=%u\n", role, stopped, generation);
#endif
            continue;
        }
        failedRole = -1;
        dpApplied = dp;
        gRoleApplied = role;
        gAppliedLane = lane;
        gAppliedGeneration = generation;
        native_usb_stats[44] = (uint32_t)role | (uint32_t)role << 8 | (uint32_t)gCoreRole << 16;
        native_usb_stats[45] = native_usb_stats[45] + 1;
        native_usb_stats[35] = ((native_usb_stats[35] & 0xffffu) + 1) | (uint32_t)(role == kRoleDevice) << 16;
#ifdef QCOM_DWC3_UKE_USB2
        printf("[usb otg] applied role=%d generation=%u power_request=%08x\n",
               role, generation, native_usb_stats[20]);
        if (role == kRoleHost) hostStats();
#endif
    }
}
#ifdef QCOM_DWC3_UKE_USB2
extern "C" void qcom_uke_dp_update(int lane, bool hpd, bool irq) {
    IOLock *lock = __atomic_load_n(&gRoleLock, __ATOMIC_ACQUIRE);
    if (!lock) return;
    IOLockLock(lock);
    const bool eligible = lane < 0 || (gRoleWanted == kRoleHost && lane == gWantedLane);
    if (eligible) {
        const bool newRoute = gUkeDpLane != lane || gUkeDpGeneration != gWantedGeneration;
        if (hpd && (newRoute || !gUkeDpHpd)) ++gUkeDpProbeRequested;
        if (hpd && irq) ++gUkeDpAttention;
        gUkeDpLane = lane;
        gUkeDpHpd = hpd;
        gUkeDpGeneration = gWantedGeneration;
        IOLockWakeup(lock, &gRoleWanted, false);
    }
    IOLockUnlock(lock);
    if (!eligible) printf("[dp route] PAN lane=%d is waiting for a matching USB host route\n", lane);
}
#endif
extern "C" void qcom_dwc3_vbus(int online) {
    gVbusOn = online != 0;
#ifdef QCOM_DWC3_UKE_USB2
    printf("[usb otg] PMIC usb_online=%d\n", gVbusOn);
#else
    cableChanged();
#endif
}
extern "C" void qcom_dwc3_host_load(int result) {native_usb_stats[46] = (uint32_t)result << 16;}
// The USB-C owner on the PMIC channel; the board's battery service owns the channel and its work loop, and calls these there.
extern "C" uint32_t qcom_ucsi_start(uint8_t *out) {return ucsi_start(&gUcsi, out);}
extern "C" void qcom_ucsi_stop() {
    gUcsi = {};
    gVbusOn = -1;
    cableChanged();
}
extern "C" bool qcom_ucsi_connection(uint32_t *generation, uint32_t *status) {
    if (gUcsi.state != kUcsiReady || !gUcsi.status_valid) return false;
    *generation = gUcsi.connection_generation;
    *status = gUcsi.status;
    return true;
}
extern "C" void qcom_ucsi_apply_connection(int lane) {
    if (gUcsi.state == kUcsiReady && gUcsi.status_valid) cableChanged(lane);
}
static void ucsiStats() {
    native_usb_stats[40] = (uint32_t)gUcsi.state | gUcsi.version << 16;
    native_usb_stats[41] = gUcsi.status;
    native_usb_stats[42] = gUcsi.cci;
    native_usb_stats[43] = gUcsi.changes | gUcsi.errors << 16;
#ifdef QCOM_DWC3_UKE_USB2
    static uint32_t lastState = ~0u, lastStatus = ~0u, lastGeneration = ~0u;
    if (lastState != native_usb_stats[40] || lastStatus != gUcsi.status || lastGeneration != gUcsi.connection_generation) {
        lastState = native_usb_stats[40];
        lastStatus = gUcsi.status;
        lastGeneration = gUcsi.connection_generation;
        printf("[usb otg] UCSI state=%d version=%04x cc=%08x status=%08x generation=%u online=%d\n",
               gUcsi.state, gUcsi.version, gUcsi.cci, gUcsi.status, gUcsi.connection_generation, gVbusOn);
    }
#endif
}
extern "C" uint32_t qcom_ucsi_receive(const uint8_t *data, uint32_t length, uint8_t *out) {
    const uint32_t n = ucsi_receive(&gUcsi, data, length, out);
    ucsiStats();
#ifndef QCOM_DWC3_UKE_USB2
    if (gUcsi.state == kUcsiReady && !n) cableChanged();
#endif
    return n;
}
extern "C" uint32_t qcom_ucsi_tick(uint8_t *out) {
    const uint32_t n = ucsi_tick(&gUcsi, out);
    ucsiStats();
    if (gUcsi.state == kUcsiFailed) qcom_ucsi_stop();
    return n;
}
extern "C" bool qcom_ucsi_polling(void) {return gUcsi.polling != 0;}
// The hardware, before the classes exist: a failure leaves the system running with USB off.
extern "C" bool qcom_dwc3_bring_up(void) {return bringUp();}
// After the module's metaclasses are registered: publish the provider and start the controller.
// For bring-up (usb=2): what the core and the SMMU see while the system stack runs, sampled about every 5 ms for the link state.
// cycle (usb=3) also takes the controller off the bus and back once, the path a cable removal runs, for an unattended host check.
#ifdef QCOM_DWC3_UKE_USB2
namespace {
IOReturn enableMuxDebug() {
    OSDictionary *matching = IOService::serviceMatching("AppleUSBDeviceMux");
    OSIterator *services = matching ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!services) return kIOReturnNotFound;

    IOReturn result = kIOReturnNotFound;
    while (OSObject *object = services->getNextObject()) {
        IOService *service = OSDynamicCast(IOService, object);
        if (!service) continue;
        OSDictionary *properties = OSDictionary::withCapacity(1);
        OSNumber *level = OSNumber::withNumber(7ULL, 32);
        if (properties && level && properties->setObject("DebugLevel", level))
            result = service->setProperties(properties);
        else
            result = kIOReturnNoMemory;
        OSSafeReleaseNULL(level);
        OSSafeReleaseNULL(properties);
        break;
    }
    OSSafeReleaseNULL(services);
    return result;
}
} // namespace
#endif
extern "C" void qcom_dwc3_diag(bool cycle) {
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000) || !gSmmu.va) return;
    const uint32_t pages = gSmmuPages, bank = gSmmuBank;
#ifdef QCOM_DWC3_UKE_USB2
    (void)cycle;
    // Capture changes quickly, without reading or acknowledging an event owned by AppleUSBXDCI.
    printf("[usb] uke XDCI state power=%u request=%08x on=%u off=%u reset=%08x\n",
           static_cast<unsigned>(gController ? gController->getPowerState() : 0), native_usb_stats[20], native_usb_stats[12],
           native_usb_stats[13], native_usb_stats[14]);
    uint32_t lastIrq = 0, lastDsts = 0xffffffffu;
    uint32_t lastLtssm = ~0u, lastOrientation = ~0u;
    unsigned ssTraceRows = 0;
    bool muxDebugEnabled = false;
    unsigned muxDebugAttempts = 0;
    // Keep the same bounded sampling rate through the 150-second reconnect test.
    // Three hundred nominal seconds cover late configd startup and both cable observations.
    for (unsigned i = 0; i < 15000; ++i) {
        if (gController && i % 50 == 0) gController->retryCablePower();
        if (!muxDebugEnabled && muxDebugAttempts < 180 && i % 50 == 0) {
            ++muxDebugAttempts;
            const IOReturn result = enableMuxDebug();
            if (result == kIOReturnSuccess) {
                muxDebugEnabled = true;
                printf("[usb flight] AppleUSBDeviceMux DebugLevel=7 applied at sample=%u attempts=%u\n",
                       i, muxDebugAttempts);
            } else if (muxDebugAttempts == 1 || muxDebugAttempts % 30 == 0) {
                printf("[usb flight] AppleUSBDeviceMux DebugLevel=7 pending result=%08x attempts=%u\n",
                       static_cast<uint32_t>(result), muxDebugAttempts);
            }
        }
        bool logPower = false, snapshot = false;
        {
            UkeUsbRegistersGuard guard;
            if (gSuperSpeed) {
                UkeUsbOrientation orientation{};
                if (uke_usb3_read_orientation(orientation)) {
                    const uint32_t selection = (orientation.gpioIO & 1) |
                        ((orientation.typecStatus & 1) << 1) | ((orientation.typecControl & 3) << 2);
                    const uint32_t link = window.va[0x164 / 4];
                    const uint32_t ltssm = (link >> 22) & 15;
                    native_usb_stats[34] = selection;
                    native_usb_stats[36] |= 1u << ltssm;
                    native_usb_stats[37] = link;
                    if (selection != lastOrientation || (ltssm != lastLtssm && ssTraceRows < 256) || i % 1000 == 0) {
                        ++ssTraceRows;
                        printf("[usb flight] orientation gpio122=%08x/%08x com=%08x/%08x pcs=%08x ltssm=%08x role=%s\n",
                               orientation.gpioConfig, orientation.gpioIO, orientation.typecControl, orientation.typecStatus,
                               orientation.pcsStatus, link, gUkeHostCore ? "host" : "device");
                    }
                    lastOrientation = selection;
                    lastLtssm = ltssm;
                }
            }
            const uint32_t irq = native_usb_stats[11], dsts = gUkeHostCore ? 0u : window.va[0x70c / 4];
            if (!gUkeHostCore && (i % 250 == 0 || (irq != lastIrq && lastIrq == 0) || (dsts & 0x7c0007u) != (lastDsts & 0x7c0007u))) {
                printf("[usb] uke sample=%u irq=%u dsts=%08x dctl=%08x ep=%08x ep0=%08x evsz=%08x evcnt=%08x fsr=%08x/%08x\n",
                       i, irq, dsts, window.va[0x704 / 4], window.va[0x720 / 4], window.va[0x80c / 4],
                       window.va[0x408 / 4], window.va[0x40c / 4], gSmmu.va[0x48 / 4],
                       gSmmu.va[(pages + bank) * 1024 + 0x58 / 4]);
                printf("[usb] uke XDCI state power=%u request=%08x on=%u off=%u reset=%08x\n",
                       static_cast<unsigned>(gController ? gController->getPowerState() : 0), native_usb_stats[20],
                       native_usb_stats[12], native_usb_stats[13], native_usb_stats[14]);
                logPower = gController != nullptr;
                if (gController && i % 1000 == 0) {
                    printf("[usb flight] core id=%08x bus=%08x/%08x gctl=%08x dcfg=%08x devten=%08x phy=%08x/%08x\n",
                           window.va[0x120 / 4], window.va[0x100 / 4], window.va[0x104 / 4],
                           window.va[0x110 / 4], window.va[0x700 / 4], window.va[0x708 / 4],
                           window.va[0x200 / 4], window.va[0x2c0 / 4]);
                    printf("[usb flight] event address=%08x:%08x size=%08x count=%08x ep0=%08x/%08x ep1=%08x/%08x\n",
                           window.va[0x404 / 4], window.va[0x400 / 4], window.va[0x408 / 4], window.va[0x40c / 4],
                           window.va[0x800 / 4], window.va[0x80c / 4], window.va[0x810 / 4], window.va[0x81c / 4]);
                    volatile uint32_t *cb = gSmmu.va + (pages + bank) * 1024;
                    printf("[usb flight] smmu global=%08x fsr=%08x fsynr=%08x far=%08x:%08x sctlr=%08x rails=%x wcd=%08x\n",
                           gSmmu.va[0x48 / 4], cb[0x58 / 4], cb[0x68 / 4], cb[0x64 / 4], cb[0x60 / 4],
                           cb[0], native_usb_stats[9], native_usb_stats[32]);
                    // FSR[10:9] describe the context format, not a transaction fault.
                    printf("[usb flight] dma fault_bits=%08x smr=%08x s2cr=%08x irq_attached=%u\n",
                           cb[0x58 / 4] & ~0x600u, gSmmu.va[(0x800 + 4 * (native_usb_stats[7] & 0xffffu)) / 4],
                           gSmmu.va[(0xc00 + 4 * (native_usb_stats[7] & 0xffffu)) / 4], gLineAttached);
                    logSession("running");
                    if (gSuperSpeed) {
                        printf("[usb flight] ss ltssm=%08x pipectl=%08x pcs_init=%08x\n",
                               window.va[0x164 / 4], window.va[0x2c0 / 4], native_usb_stats[33]);
                    }
                    uke_usb2_log_phy();
                    snapshot = true;
                }
            }
            lastIrq = irq;
            lastDsts = dsts;
        }
        if (logPower) gController->logPowerState();
        if (snapshot) uke_usb_pm_snapshot(gController);
        IOSleep(20);
    }
    printf("[usb flight] complete samples=15000 irq=%u power=%u mux_debug=%u attempts=%u\n", native_usb_stats[11],
           static_cast<unsigned>(gController ? gController->getPowerState() : 0), muxDebugEnabled, muxDebugAttempts);
    window.map->release();
#else
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
#endif
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
        QcomDwc3Xdci *controller = OSTypeAlloc(QcomDwc3Xdci);
        if (!controller || !controller->init()) {stage(-14); break;}
        if (!controller->attach(nub)) {stage(-15); break;}
#ifdef QCOM_DWC3_UKE_USB2
        QcomDwc3Nub::route(nub);
#endif
        const bool started = controller->start(nub);
        native_usb_stats[19] = started;
        if (!started) {
#ifdef QCOM_DWC3_UKE_USB2
            QcomDwc3Nub::route(nullptr);
            controller->detach(nub);
            controller->release();
#endif
            stage(-16);
            break;
        }
        stage(11);
        gController = controller;
        gDeviceNub = nub;
        gActiveNub = nub;
#ifdef QCOM_DWC3_UKE_USB2
        IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
        native_usb_stats[20] = (uint32_t)controller->cable(true);
        if (uke_usb_otg_enabled()) {
            thread_t thread = nullptr;
            IOLock *lock = IOLockAlloc();
            if (lock && mapRegion(gGlobals, kAppleWindow, 0x1000) && mapRegion(gXhciRegs, kCore, 0x8000)) {
                gRoleApplied = gRoleWanted = kRoleDevice;
                __atomic_store_n(&gRoleLock, lock, __ATOMIC_RELEASE);
                if (kernel_thread_start(roleThread, nullptr, &thread) == KERN_SUCCESS) {
                    thread_deallocate(thread);
                    wantRole(kRoleDevice);
                } else {
                    // A PMIC callback may already hold a reference to this lock.
                    __atomic_store_n(&gRoleLock, nullptr, __ATOMIC_RELEASE);
                    printf("[usb otg] role worker unavailable\n");
                }
            } else {
                if (lock) IOLockFree(lock);
                OSSafeReleaseNULL(gGlobals.map);
                OSSafeReleaseNULL(gXhciRegs.map);
                printf("[usb otg] role registers unavailable\n");
            }
        }
        stage(12);
        return;
#else
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
#endif
    } while (false);
}
