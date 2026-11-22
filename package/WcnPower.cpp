/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared WCN6750 power ownership for Xiaomi Pad 7 (uke / SM7675 "cliffs").
 *
 * Implements the upstream WcnPower.h interface (sheng_wcn_power_get / put / owners)
 * over uke's verified hardware power graph:
 *   1. PMK8550 sleep clock on SPMI v7 EE 0, SID 0, PPID 0x0ba (gpio3, func 1, VIN0, output, no bias),
 *      reusing BootResetCore.h's SPMI arbiter helpers (verified in Allenwdk/UniXNU-uke WifiProbe.cpp).
 *   2. Five unique RPMh VRM rails backing the eight OEM bt_wcn6750 supplies in live-os4.dts:
 *        ldob7 (vddio)                                 1800 mV (PMXR2230 L7B)
 *        smpb3 (vddaon/vddbtcxmx/vddrfacmn/vddrfa0p8)   984 mV (PMXR2230 S3B)
 *        smpb1 (vddrfa1p7)                             1856 mV (PMXR2230 S1B)
 *        smpb2 (vddrfa1p2)                             1256 mV (PMXR2230 S2B)
 *        smpe3 (vddrfa2p2)                             1600 mV (PMR735A  S3E)
 *      Submitted through SM7675 apps RSC DRV-2 (0x17a20000, tcs-offset 0xd00, v3.2) using
 *      48 KiB cmd-db reads (cliffs aborts past 0xc000) and the strict __tcs_set_trigger()
 *      clear order (clear TRIGGER, then clear ENABLE). Each rail packs mode+voltage+enable
 *      into one 3-command TCS request, rotating active TCS 0 and 1 (USB owns TCS 2). */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <stdint.h>
#include "BootResetCore.h"
#include "WcnPower.h"

namespace {
struct Window {
    IOMemoryMap *map;
    volatile uint32_t *va;
};

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

void releaseWindow(Window &w) {
    if (w.map) w.map->release();
    w = {};
}

uint32_t r32(const Window &w, uint32_t offset) { return w.va[offset / 4]; }
void w32(const Window &w, uint32_t offset, uint32_t value) {
    w.va[offset / 4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Self-contained cmd-db lookup (48 KiB max on SM7675 cliffs; reads past 0xc000 abort). */
constexpr uint32_t kCmdDbMagic = 0x0c0330dbu;
constexpr uint64_t kCmdDbBase = 0x81c60000ULL;
constexpr size_t kCmdDbSize = 0xc000u;

uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }

bool cmdDbFind(const uint8_t *db, size_t bytes, const char *name, uint32_t *address) {
    if (!db || !name || !address || bytes < 144 || le32(db + 4) != kCmdDbMagic) return false;
    char id[8] = {0};
    for (unsigned i = 0; i < 8 && name[i]; ++i) id[i] = name[i];
    for (unsigned s = 0; s < 8; ++s) {
        const uint8_t *slave = db + 8 + 16 * s;
        if (!le16(slave)) break;
        const size_t heads = 144u + le16(slave + 2);
        const size_t count = le16(slave + 6);
        if (heads > bytes || count * 24 > bytes - heads) return false;
        for (size_t e = 0; e < count; ++e) {
            const uint8_t *entry = db + heads + 24 * e;
            if (!memcmp(entry, id, 8)) {
                uint32_t addr = le32(entry + 16);
                if (!addr) return false;
                *address = addr;
                return true;
            }
        }
    }
    return false;
}

/* PMK8550 sleep clock: SPMI v7 EE 0, SID 0, PPID 0x0ba (gpio3, func 1, output, no bias, VIN0). */
int sleepClockRead(volatile uint32_t *observer, const struct br_arbiter *arb, int apid, uint8_t *values) {
    static const uint8_t regs[] = {0x04, 0x05, 0x40, 0x41, 0x42, 0x44, 0x46};
    for (unsigned i = 0; i < sizeof(regs); ++i) {
        if (br_read8(observer, arb, apid, regs[i], &values[i])) return -EIO;
    }
    return 0;
}

bool sleepClockConfigured(const uint8_t *values) {
    return (values[2] & 3u) == 1u && (values[3] & 7u) == 0u &&
           (values[4] & 7u) == 5u && values[5] == 2u && (values[6] & 0x80u);
}

int pmicPrepare() {
    constexpr uint32_t kSleepClockPpid = 0x0ba;
    Window core = {}, cnfg = {}, obs = {}, chn = {};
    int rc = 0;
    if (!mapWindow(core, 0xc400000ULL, 0x3000) || !mapWindow(cnfg, 0xc42d000ULL, 0x4000)) {
        rc = -ENOMEM;
        goto done;
    }
    {
        struct br_arbiter arb = {core.va, cnfg.va, 0};
        const uint32_t version = core.va[0];
        const uint32_t apids = core.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
        if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !apids || apids > 1024) {
            rc = -ENOTSUP;
            goto done;
        }
        int apid = -1, owner = -1;
        for (uint32_t i = 0; i < apids; ++i) {
            const uint32_t map = core.va[(BR_APID_MAP + 4 * i) / 4];
            if (!map || ((map >> 8) & 0xfff) != kSleepClockPpid) continue;
            const int candidateOwner = (int)(cnfg.va[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
            if (apid < 0 || candidateOwner == arb.ee) {
                apid = (int)i;
                owner = candidateOwner;
            }
        }
        if (apid < 0) { rc = -ENOENT; goto done; }
        const uint64_t obsSize = (uint64_t(BR_OBS_APID_STRIDE) * apids + 0xfffULL) & ~0xfffULL;
        const uint64_t chnSize = uint64_t(BR_CHNL_APID_STRIDE) * apids;
        if (!mapWindow(obs, 0xc440000ULL, obsSize) || !mapWindow(chn, 0xc500000ULL, chnSize)) {
            rc = -ENOMEM;
            goto done;
        }
        uint8_t before[7] = {};
        if (sleepClockRead(obs.va, &arb, apid, before)) { rc = -EIO; goto done; }
        if (before[0] != 0x10 || before[1] < 0x10 || before[1] > 0x13) { rc = -ENOTSUP; goto done; }
        if (sleepClockConfigured(before)) { rc = 0; goto done; }
        if (owner != arb.ee) { rc = -EPERM; goto done; }
        static const uint8_t config[][2] = {{0x42, 5}, {0x41, 0}, {0x44, 2}, {0x40, 1}, {0x46, 0x80}};
        for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); ++i) {
            if (br_write8(chn.va, apid, config[i][0], config[i][1])) { rc = -EIO; goto done; }
        }
        uint8_t after[7] = {};
        if (sleepClockRead(obs.va, &arb, apid, after) || !sleepClockConfigured(after)) rc = -EIO;
    }
done:
    releaseWindow(chn);
    releaseWindow(obs);
    releaseWindow(cnfg);
    releaseWindow(core);
    if (rc) printf("[wcn-power] sleep clock rc=%d\n", rc);
    return rc;
}

struct WcnRail {
    const char *name;
    uint32_t mv;
};

/* Five unique RPMh rails backing uke's eight bt_wcn6750 supplies (live-os4.dts:22922-22947). */
constexpr WcnRail kUkeWcnRails[] = {
    {"ldob7", 1800},
    {"smpb3", 984},
    {"smpb1", 1856},
    {"smpb2", 1256},
    {"smpe3", 1600},
};

/* SM7675 apps RSC DRV-2 (0x17a20000, tcs-offset 0xd00, v3.2). */
constexpr uint64_t kRscBase = 0x17a20000ULL;
constexpr uint32_t kRscTcsOffset = 0xd00u;
constexpr uint32_t kTcsStride = 672u;
constexpr uint32_t kCmdStride = 24u;
constexpr uint32_t kIrqClear = 0x08u;
constexpr uint32_t kTcsWaitForCmpl = 0x20u;
constexpr uint32_t kTcsControl = 0x24u;
constexpr uint32_t kTcsStatus = 0x28u;
constexpr uint32_t kTcsCmdEnable = 0x2cu;
constexpr uint32_t kCmdMsgId = 0x34u;
constexpr uint32_t kCmdAddr = 0x38u;
constexpr uint32_t kCmdData = 0x3cu;
constexpr uint32_t kCmdStatus = 0x40u;
constexpr uint32_t kMsgIdWriteResp = 8u | (1u << 8) | (1u << 16);
constexpr uint32_t kAmcEnable = 1u << 16;
constexpr uint32_t kAmcTrigger = 1u << 24;
constexpr uint32_t kCmdDoneMask = (1u << 8) | (1u << 16);

bool submitTcs(const Window &rsc, unsigned tcsIndex, const uint32_t *addr, const uint32_t *val, unsigned count) {
    if (!count || count > 4) return false;
    const uint32_t base = kRscTcsOffset + tcsIndex * kTcsStride;
    if (!r32(rsc, base + kTcsStatus)) return false;
    const uint32_t mask = (1u << count) - 1u;
    w32(rsc, base + kTcsCmdEnable, 0);
    w32(rsc, base + kTcsWaitForCmpl, 0);
    for (unsigned c = 0; c < count; ++c) {
        const uint32_t cmd = base + c * kCmdStride;
        w32(rsc, cmd + kCmdMsgId, kMsgIdWriteResp);
        w32(rsc, cmd + kCmdAddr, addr[c]);
        w32(rsc, cmd + kCmdData, val[c]);
    }
    w32(rsc, base + kTcsWaitForCmpl, mask);
    w32(rsc, base + kTcsCmdEnable, mask);
    /* Strict __tcs_set_trigger() clear order: clear TRIGGER first, then ENABLE. */
    w32(rsc, base + kTcsControl, r32(rsc, base + kTcsControl) & ~kAmcTrigger);
    w32(rsc, base + kTcsControl, r32(rsc, base + kTcsControl) & ~kAmcEnable);
    w32(rsc, base + kTcsControl, kAmcEnable);
    w32(rsc, base + kTcsControl, kAmcEnable | kAmcTrigger);
    bool ok = true;
    for (unsigned c = 0; c < count; ++c) {
        const uint32_t off = base + c * kCmdStride + kCmdStatus;
        unsigned waited = 0;
        while ((r32(rsc, off) & kCmdDoneMask) != kCmdDoneMask && waited < 5000) {
            IODelay(1);
            ++waited;
        }
        if ((r32(rsc, off) & kCmdDoneMask) != kCmdDoneMask) { ok = false; break; }
    }
    w32(rsc, base + kTcsCmdEnable, 0);
    w32(rsc, base + kTcsWaitForCmpl, 0);
    w32(rsc, kRscTcsOffset + kIrqClear, 1u << tcsIndex);
    return ok;
}

int powerRails(bool shared, bool on = true) {
    if (!shared) return 0;
    unsigned railsArg = 1;
    PE_parse_boot_argn("bt-rails", &railsArg, sizeof(railsArg));
    if (!railsArg) return 0;

    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!db) return -ENOMEM;
    Window dbw = {}, rsc = {};
    int rc = 0;
    if (!mapWindow(dbw, kCmdDbBase, kCmdDbSize)) { rc = -ENOMEM; goto out; }
    for (size_t i = 0; i < kCmdDbSize; i += 4) {
        uint32_t w = r32(dbw, (uint32_t)i);
        memcpy(db + i, &w, 4);
    }
    releaseWindow(dbw);
    if (!mapWindow(rsc, kRscBase, 0x10000)) { rc = -ENOMEM; goto out; }
    if (((r32(rsc, 0) >> 16) & 0xffu) != 3u) { rc = -ENOTSUP; goto out; }

    {
        const unsigned count = (unsigned)(sizeof(kUkeWcnRails) / sizeof(kUkeWcnRails[0]));
        for (unsigned step = 0; step < count; ++step) {
            const unsigned i = on ? step : (count - 1u - step);
            uint32_t address = 0;
            if (!cmdDbFind(db, kCmdDbSize, kUkeWcnRails[i].name, &address)) {
                printf("[wcn-power] missing cmd-db rail %s\n", kUkeWcnRails[i].name);
                rc = -ENOENT;
                break;
            }
            /* Rotate active TCS 0 and 1 (TCS 2 belongs to USB; <= 3 requests per TCS). */
            const unsigned tcs = step & 1u;
            bool ok;
            if (on) {
                const uint32_t addrs[3] = {address + 8u, address, address + 4u};
                const uint32_t vals[3] = {7u, kUkeWcnRails[i].mv, 1u};
                ok = submitTcs(rsc, tcs, addrs, vals, 3);
            } else {
                const uint32_t addrs[1] = {address + 4u};
                const uint32_t vals[1] = {0u};
                ok = submitTcs(rsc, tcs, addrs, vals, 1);
            }
            if (!ok) {
                printf("[wcn-power] rpmh rail %s on=%u failed on tcs=%u\n", kUkeWcnRails[i].name, (unsigned)on, tcs);
                rc = -EIO;
                break;
            }
        }
    }
out:
    releaseWindow(dbw);
    releaseWindow(rsc);
    IOFreeData(db, kCmdDbSize);
    return rc;
}

/* ---- WCN ownership state machine (host-tested verbatim by scripts/check-wcn-power.py) ---- */
IOLock *gWcnLock;
unsigned gWcnOwners;
IOLock *wcnLock() {
    IOLock *lock = __atomic_load_n(&gWcnLock, __ATOMIC_ACQUIRE);
    if (lock) return lock;
    IOLock *candidate = IOLockAlloc();
    if (!candidate) return nullptr;
    if (!__atomic_compare_exchange_n(&gWcnLock, &lock, candidate, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        IOLockFree(candidate);
        return lock;
    }
    return candidate;
}
extern "C" int sheng_wcn_power_get(unsigned radio) {
    if (radio > SHENG_WCN_BLUETOOTH) return EINVAL;
    IOLock *lock = wcnLock();
    if (!lock) return ENOMEM;
    IOLockLock(lock);
    const unsigned bit = 1u << radio;
    if (gWcnOwners & bit) { IOLockUnlock(lock); return 0; }
    int error = 0;
    bool railsAttempted = false;
    if (!gWcnOwners && pmicPrepare()) error = EIO;
    if (!error) {
        railsAttempted = !gWcnOwners;
        if (powerRails(!gWcnOwners, true)) error = EIO;
    }
    if (!error) gWcnOwners |= bit;
    else if (railsAttempted) (void)powerRails(true, false);
    IOLockUnlock(lock);
    return error;
}
extern "C" int sheng_wcn_power_put(unsigned radio) {
    if (radio > SHENG_WCN_BLUETOOTH) return EINVAL;
    IOLock *lock = wcnLock();
    if (!lock) return ENOMEM;
    IOLockLock(lock);
    const unsigned bit = 1u << radio;
    int error = 0;
    if ((gWcnOwners & bit) && !(gWcnOwners & ~bit)) {
        if (powerRails(true, false)) error = EIO;
    }
    if (!error) gWcnOwners &= ~bit;
    IOLockUnlock(lock);
    return error;
}
extern "C" unsigned sheng_wcn_power_owners(void) {
    IOLock *lock = wcnLock();
    if (!lock) return 0;
    IOLockLock(lock);
    unsigned owners = gWcnOwners;
    IOLockUnlock(lock);
    return owners;
}
/* ---- end WCN ownership state machine ---- */
} // namespace
