// SPDX-License-Identifier: BSD-2-Clause-Patent
// Standalone QUPv3 wrap0 SE0 (i2c@a80000) GENI I2C bus for WCD939x USB-C switch routing
// (uke_backlight_with_wcd_bus) when AppleARMBacklight is not linked, plus weak stubs for
// optional DisplayPort / PM trace hooks referenced by UkeUsbBoard.cpp and QcomDwc3.cpp.
#include "UkeUsbBoard.h"
#include "GeniI2c.h"
#include "NativeExternalDisplay.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>

namespace {
constexpr uint64_t kGcc = 0x100000, kSe0 = 0xa80000;
constexpr uint32_t kGccVote = 0x52008;
constexpr uint32_t kVoteBits = (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22);
constexpr uint32_t kHalt[] = {0x23018, 0x23008, 0x23000, 0x23004, 0x18004};

volatile uint32_t *gSe0Va = nullptr;
IOLock *gWcdLock = nullptr;
bool gSe0Ready = false;

uint32_t rd(volatile uint32_t *va, uint32_t off) { return va[off / 4]; }
void wr(volatile uint32_t *va, uint32_t off, uint32_t v) {
    va[off / 4] = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t se0Read(void *ctx, uint32_t reg) { return rd(static_cast<volatile uint32_t *>(ctx), reg); }
void se0Write(void *ctx, uint32_t reg, uint32_t v) { wr(static_cast<volatile uint32_t *>(ctx), reg, v); }

volatile uint32_t *mapPa(uint64_t pa, size_t size) {
    IODeviceMemory *m = IODeviceMemory::withRange(pa, size);
    if (!m) return nullptr;
    IOMemoryMap *map = m->map(kIOMapAnywhere | kIOMapInhibitCache);
    m->release();
    return map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
}

bool ensureSe0() {
    if (gSe0Ready) return true;
    volatile uint32_t *gcc = mapPa(kGcc, 0x60000);
    volatile uint32_t *se0 = mapPa(kSe0, 0x4000);
    if (!gcc || !se0) return false;
    // GPIOs 52 and 53: SE0 I2C SDA/SCL (function 1, 2 mA, pull-up).
    const unsigned pins[] = {52u, 53u};
    for (unsigned pin : pins) {
        volatile uint32_t *tlmm = mapPa(0x0f100000ull + 0x1000ull * pin, 0x1000);
        if (!tlmm) return false;
        uint32_t cfg = rd(tlmm, 0);
        cfg &= ~((15u << 2) | (7u << 6) | (1u << 9) | 3u);
        cfg |= (1u << 2) | 3u;
        wr(tlmm, 0, cfg);
    }
    wr(gcc, kGccVote, rd(gcc, kGccVote) | kVoteBits);
    for (uint32_t halt : kHalt) {
        if (rd(gcc, halt) & (1u << 1)) continue;
        unsigned us = 0;
        while ((rd(gcc, halt) & (1u << 31)) && us < 200) { IODelay(1); ++us; }
        if (us == 200) return false;
    }
    gSe0Va = se0;
    gSe0Ready = true;
    return true;
}
} // namespace

__attribute__((weak))
IOReturn uke_backlight_with_wcd_bus(UkeWcdBusAction action, void *context) {
    if (!action) return kIOReturnBadArgument;
    if (!gWcdLock) gWcdLock = IOLockAlloc();
    if (!gWcdLock) return kIOReturnNoMemory;
    IOLockLock(gWcdLock);
    IOReturn ret = kIOReturnNotReady;
    if (ensureSe0()) {
        GeniI2c bus{};
        const GeniIo io = {se0Read, se0Write, const_cast<uint32_t *>(gSe0Va)};
        uint32_t config = 0;
        if (geni_i2c_prepare(&bus, &io, 0x0e, &config) == 0) {
            const uint32_t depth = bus.rx_fifo_words;
            if (depth > 1) {
                wr(gSe0Va, GENI_RX_WATERMARK, depth - 1);
                wr(gSe0Va, GENI_RX_RFR_WATERMARK, depth);
            }
            wr(gSe0Va, GENI_SE_IRQ_EN, rd(gSe0Va, GENI_SE_IRQ_EN) & ~GENI_SE_M_IRQ_EN);
            wr(gSe0Va, GENI_M_IRQ_CLEAR, rd(gSe0Va, GENI_M_IRQ_STATUS));
            ret = action(bus, context) ? kIOReturnSuccess : kIOReturnIOError;
        } else {
            ret = kIOReturnNoDevice;
        }
    }
    IOLockUnlock(gWcdLock);
    return ret;
}

__attribute__((weak))
IOReturn uke_dp_set_external_display(const NativeExternalDisplayLink *) {
    return kIOReturnUnsupported;
}

extern "C" __attribute__((weak))
bool native_legacy_dp_debug() {
    return false;
}

extern "C" __attribute__((weak))
void uke_usb_pm_snapshot(IOService *) {}

extern "C"
uint32_t uke_ufs_dpu_map_external(unsigned, const uint64_t *, unsigned) {
    return 0;
}

extern "C" __attribute__((weak))
uint32_t native_ufs_dpu_live(unsigned) {
    return 0;
}

extern "C" __attribute__((weak))
bool native_legacy_panel_dma_left_ready() {
    return false;
}

extern "C" __attribute__((weak))
IOReturn native_legacy_panel_route_dma(bool) {
    return kIOReturnUnsupported;
}
