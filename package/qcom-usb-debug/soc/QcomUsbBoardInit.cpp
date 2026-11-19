// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The board half of USB device mode: rails, core power, the USB2 reference clock, the controller reset and
// the PMIC repeater. Extracted verbatim (values, order and failure codes unchanged) from
// Allenwdk/UniXNU-uke package/UkeUsbBoard.cpp at 55909f4 — the tree that first enumerated this board in
// device mode (2026-11-15 22:51:37: host saw 05ac:12ab, 480 Mbps, configuration 1; docs/USB2-20261115.md).
//
// Why it is not that file: UkeUsbBoard.cpp also carries the DisplayPort/QMP half and includes the display
// stack (NativeExternalDisplay.h, UkeDpDpu.h, UkeDpLinkPolicy.h, UkeDpVideoMode.h) plus the WCD939x route
// through the backlight's GENI bus, none of which this port runs. Everything below is ABI-free register
// work, so it ports unchanged onto this kernel's own UFS/polling DWC3 path; the WCD route is stage 2
// because the firmware already leaves the TypeC switch on the USB position (their fix exists because their
// flow re-initialises it).
//
// Failure codes are upstream's, so the panel's QCOMUSB line reads the same way it does on their build:
//   -1 GCC window, -2 rails/RSC, -3 core GDSC, -4 clocks or TCSR clkref or controller reset,
//   -5 (their USB3-only step), -6 repeater, -7 PHY.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <string.h>

#include "BootResetCore.h"
#include "QcomRpmh.h"
#include "QcomUsbBoardInit.h"

namespace {

void sync() { __asm__ volatile("dsb sy" ::: "memory"); }

struct Registers {
  IOMemoryMap *mapping = nullptr;
  volatile uint32_t *base = nullptr;
  ~Registers() { OSSafeReleaseNULL(mapping); }
  bool map(uint64_t pa, uint64_t bytes) {
    auto *range = IODeviceMemory::withRange(pa, bytes);
    mapping = range ? range->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(range);
    base = mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
    return base != nullptr;
  }
  uint32_t read(unsigned offset) const { return base[offset / 4]; }
  void write(unsigned offset, uint32_t value) const { base[offset / 4] = value; sync(); }
  void update(unsigned offset, uint32_t mask, uint32_t value) const {
    write(offset, (read(offset) & ~mask) | value);
  }
  bool wait(unsigned offset, uint32_t mask, uint32_t value, unsigned count = 200) const {
    for (unsigned i = 0; i < count; ++i) {
      if ((read(offset) & mask) == value) return true;
      IODelay(10);
    }
    return false;
  }
};

constexpr unsigned kCoreBcr = 0x39000;
constexpr unsigned kCoreGdsc = 0x39004;
constexpr unsigned kCoreClocks[] = {0x39088, 0x39018, 0x39024, 0x39028, 0x3908c};

Registers gGcc;

// rpmh-rsc.c extracts the major version at bits 23:16; the DRV-2 selection is the aperture, bits 15:8 the
// minor version.
constexpr bool supportedRscVersion(uint32_t id) { return ((id >> 16) & 0xff) == 3; }
static_assert(supportedRscVersion(0x00030200), "Uke apps RSC DRV-2 reports version 3.2");
static_assert(!supportedRscVersion(0x00020200), "USB uses the version 3 status offsets");

bool refClock() {
  // The OEM tcsrcc-pineapple driver uses a normal MMIO regmap and clk_branch2_ops: an SCM request for the
  // same register answers -2 on this SEC firmware (their 2026-11-15 readback), MMIO works.
  constexpr uintptr_t tcsrccBase = 0x01f40000;
  constexpr unsigned usb2ClkrefOffset = 0x000b1118;
  constexpr uintptr_t pa = tcsrccBase + usb2ClkrefOffset;
  static_assert(pa == 0x01ff1118, "Cliffs USB2 TCSR gate address");
  constexpr uintptr_t page = pa & ~uintptr_t(0xfff);
  constexpr unsigned reg = static_cast<unsigned>(pa - page);
  constexpr uint32_t enable = 1u;
  Registers tcsr;
  if (!tcsr.map(page, 0x1000)) {
    IOLog("[usb] uke TCSR clkref pa=%08x map failed\n", static_cast<uint32_t>(pa));
    return false;
  }
  const uint32_t before = tcsr.read(reg);
  if (!(before & enable)) {
    tcsr.write(reg, before | enable);
    IODelay(10);  // BRANCH_HALT_DELAY
  }
  const uint32_t after = tcsr.read(reg);
  IOLog("[usb] uke TCSR clkref pa=%08x before=%08x after=%08x\n", static_cast<uint32_t>(pa), before, after);
  return (after & enable) != 0;
}

bool voteRails(bool superSpeed) {
  Registers db, rsc;
  constexpr unsigned kDbSize = 0x20000, kDrvOffset = 0xd00, kUsbTcs = 2;
  if (!db.map(0x81c60000, kDbSize) || !rsc.map(0x17a20000, 0x10000)) return false;
  auto *copy = static_cast<uint8_t *>(IOMallocData(kDbSize));
  if (!copy) return false;
  for (unsigned i = 0; i < kDbSize; i += 4) {
    uint32_t word = db.read(i);
    memcpy(copy + i, &word, sizeof(word));
  }
  const uint32_t rscId = rsc.read(0);
  const auto layout = qcom_rsc_layout(rscId);
  const unsigned tcs = kDrvOffset + kUsbTcs * layout.tcs_stride;
  constexpr unsigned kV3TcsStatus = 0x28, kV3CmdStatus = 0x40;
  constexpr unsigned kTriggered = 1u << 24, kIssuedAndComplete = (1u << 8) | (1u << 16);
  bool ok = supportedRscVersion(rscId);
  auto submit = [&](const char *resource, const uint32_t *addr, const uint32_t *val, unsigned count) {
    const uint32_t status = rsc.read(tcs + kV3TcsStatus);
    const uint32_t control = rsc.read(tcs + layout.control);
    if (!status || (control & kTriggered)) {
      IOLog("[usb] uke rpmh busy resource=%s tcs=%u status=%08x control=%08x\n", resource, kUsbTcs, status,
            control);
      return false;
    }
    QcomRpmhWrite writes[20];
    const unsigned n = qcom_rpmh_active_request(&layout, kUsbTcs, addr, val, count, writes, 20);
    if (!n) return false;
    for (unsigned i = 0; i < n; ++i) rsc.write(kDrvOffset + writes[i].offset, writes[i].value);
    for (unsigned i = 0; i < count; ++i) {
      const unsigned offset = tcs + kV3CmdStatus + i * layout.cmd_stride;
      if (!rsc.wait(offset, kIssuedAndComplete, kIssuedAndComplete, 5000)) {
        IOLog("[usb] uke rpmh timeout resource=%s cmd=%u address=%08x status=%08x tcs=%08x irq=%08x\n",
              resource, i, addr[i], rsc.read(offset), rsc.read(tcs + kV3TcsStatus),
              rsc.read(kDrvOffset + layout.irq_status));
        return false;
      }
    }
    const unsigned done = qcom_rpmh_finish(&layout, kUsbTcs, writes, 20);
    for (unsigned i = 0; i < done; ++i) rsc.write(kDrvOffset + writes[i].offset, writes[i].value);
    return true;
  };
  // The effective rails of the OEM's eusb2 node: LDOB2 0.88 V, LDOB4 1.2 V, LDOB7 1.8 V, LDOB17 3.075 V.
  const struct {
    const char *name;
    unsigned mv;
    bool superSpeed;
  } rails[] = {
      {"ldob2", 880, false}, {"ldob4", 1200, false}, {"ldob7", 1800, false},
      {"ldob17", 3075, false}, {"ldob3", 880, true},
  };
  for (unsigned i = 0; ok && i < sizeof(rails) / sizeof(*rails); ++i) {
    if (rails[i].superSpeed != superSpeed) continue;
    QcomRpmhResource rail{};
    const int lookup = qcom_cmd_db_find(copy, kDbSize, rails[i].name, &rail);
    ok = lookup == 0;
    if (!ok) {
      IOLog("[usb] uke cmd-db resource=%s status=%d\n", rails[i].name, lookup);
    } else {
      const uint32_t addresses[] = {rail.address + 8, rail.address, rail.address + 4};
      const uint32_t values[] = {7, rails[i].mv, 1};
      ok = submit(rails[i].name, addresses, values, 3);
    }
  }
  QcomRpmhResource xo{}, sn2{};
  if (ok && !qcom_cmd_db_find(copy, kDbSize, "xo.lvl", &xo) && !qcom_cmd_db_find(copy, kDbSize, "SN2", &sn2) &&
      sn2.level_count >= 3) {
    const auto unit = sn2.levels[0] | (uint32_t)sn2.levels[1] << 16;
    const uint64_t peak = superSpeed ? 2500000 : 60000;
    const uint64_t scaled = unit ? (((peak * sn2.levels[2] + 15) / 16) * 1000 + unit - 1) / unit : 0;
    const uint32_t vote = scaled > 0x3fff ? 0x3fff : static_cast<uint32_t>(scaled);
    const uint32_t addresses[] = {xo.address, sn2.address};
    const uint32_t values[] = {3, 1u << 30 | 1u << 29 | vote << 14 | vote};
    ok = vote && submit("xo.lvl/SN2", addresses, values, 2);
  } else {
    ok = false;
  }
  IOFreeData(copy, kDbSize);
  IOLog("[usb] uke rails rsc=%08x complete=%u\n", rscId, ok);
  return ok;
}

bool resetController() {
  if (!gGcc.base) return false;
  // dwc3_msm_link_clk_reset: gate the link clocks, assert the external block reset, then restore clocks.
  for (unsigned offset : kCoreClocks) gGcc.update(offset, 1, 0);
  gGcc.update(kCoreBcr, 1, 1);
  const bool asserted = gGcc.wait(kCoreBcr, 1, 1);
  IOSleep(1);
  gGcc.update(kCoreBcr, 1, 0);
  const bool released = gGcc.wait(kCoreBcr, 1, 0);
  IODelay(1);
  bool clocked = true;
  for (unsigned offset : kCoreClocks) {
    gGcc.update(offset, 1, 1);
    clocked = gGcc.wait(offset, 0x80000001u, 1) && clocked;
  }
  IOSleep(10);
  IOLog("[usb] uke BCR assert=%u release=%u clocks=%u\n", asserted, released, clocked);
  return asserted && released && clocked;
}

// PMIC arbiter: the eUSB2 repeater of PM7550BA SID 7 sits at 0xfd00, and its device-mode tuning is the OEM's.
bool repeater(bool host) {
  Registers core, owner, observer, channel;
  if (!core.map(0xc400000, 0x3000) || !owner.map(0xc42d000, 0x4000)) return false;
  const br_arbiter arb{core.base, owner.base, 0};
  const uint32_t version = core.read(0);
  const unsigned count = core.read(BR_FEATURES) & BR_FEATURES_APIDS_MASK;
  if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !count || count > 1024) return false;
  unsigned apid = 0;
  while (apid < count) {
    if (((core.read(BR_APID_MAP + 4 * apid) >> 8) & 0xfff) == 0x7fd &&
        (owner.read(BR_CNFG_OWNER + 4 * apid) & 7) == arb.ee)
      break;
    ++apid;
  }
  if (apid == count || !observer.map(0xc440000, BR_OBS_APID_STRIDE * count) ||
      !channel.map(0xc500000, BR_CHNL_APID_STRIDE * count))
    return false;
  auto read = [&](uint8_t reg, uint8_t &value) {
    return br_read8(observer.base, &arb, apid, reg, &value) == 0;
  };
  auto write = [&](uint8_t reg, uint8_t value) {
    return br_write8(channel.base, apid, reg, value) == 0;
  };
  uint8_t value = 0;
  if (!read(0x46, value) || !write(0x46, value | 0x80)) return false;
  // Device and host properties of the effective eusb2-repeater@fd00 node.
  constexpr uint8_t deviceTuning[] = {0, 0x0c, 3, 7, 2, 3, 0, 4, 2, 0, 3, 2};
  constexpr uint8_t hostTuning[] = {0, 0x08, 5, 7, 2, 1, 0, 6, 2, 0, 3, 2};
  const auto &tuning = host ? hostTuning : deviceTuning;
  for (unsigned i = 0; i < sizeof tuning; ++i)
    if (!write(0x50 + i, tuning[i])) return false;
  // Program the forced value before enabling its override.
  constexpr uint8_t forceRegisters[] = {0xed, 0xe8};
  for (uint8_t reg : forceRegisters) {
    const uint8_t force = host ? 0x40 : 0;
    if (!read(reg, value) || !write(reg, (value & ~0x40u) | force) || !read(reg, value) ||
        (value & 0x40u) != force)
      return false;
  }
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    if (!read(8, value)) return false;
    if (value & 0x80) return true;
    IODelay(100);
  }
  return false;
}

}  // namespace

int qcom_usb_board_init(void) {
  if (!gGcc.map(0x100000, 0x53000)) return -1;
  if (!voteRails(false)) return -2;
  gGcc.update(kCoreGdsc, 1, 0);
  if (!gGcc.wait(kCoreGdsc, 1u << 31, 1u << 31, 10000)) return -3;
  // Core at the OEM's 200 MHz source, GPLL0/3. Preserve mock UTMI's firmware divisor.
  gGcc.update(0x39030, 0x71f, 0x105);
  gGcc.update(0x3902c, 0, 1);
  if (!gGcc.wait(0x3902c, 1, 0) || !refClock() || !resetController()) return -4;
  if (!repeater(false)) return -6;
  IOLog("[usb] uke board init ok (rails + core + clkref + reset + repeater)\n");
  return 0;
}
