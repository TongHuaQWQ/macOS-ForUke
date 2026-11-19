/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * USB2 device-mode board bring-up for SM7675 (uke / cliffs).
 *
 * The five stages that make the controller's link real: PMIC rails through the apps RSC, the USB core's GDSC
 * and link clocks in GCC, the USB2 reference clock gate in TCSR, the controller reset, and the eUSB2 repeater
 * on PM7550BA. Values, order and failure codes are taken unchanged from Allenwdk/UniXNU-uke
 * package/UkeUsbBoard.cpp (55909f4) — the tree whose 2026-11-15 record (docs/USB2-20261115.md) is the first
 * time this board enumerated in device mode at all (host saw 05ac:12ab, 480 Mbps, configuration 1).
 *
 * Only the register half is taken: their file also carries the DisplayPort/QMP half, the display stack, and
 * the WCD939x route through the backlight's GENI bus (whose SE0 is shared with the panel's backlight chip).
 * None of that is ABI-bound, so this ports onto the kernel's own DWC3 path; WCD routing is stage 2, because
 * the firmware already leaves the TypeC switch on the USB position.
 *
 * Resources and where each comes from are listed in QcomUsbBoardInit.h. No SCM, no SPMI driver: everything
 * is MMIO through the windows above, which is exactly why upstream's SCM attempt (-2) was replaced by the
 * TCSR MMIO path in their 2026-11-15 fix.
 */
#include <stdint.h>
#include <string.h>

#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>

/* This component's units import only the kernel symbols build.py allows (IOLog is on that list; IODelay,
 * IOSleep and printf are not). Both helpers are therefore the same two the existing kernel unit uses: IOLog
 * for output, and a CNTVCT_EL0 busy wait for delay. */
extern void IOLog(const char *format, ...);

static uint64_t board_counter(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
  return value;
}

static uint64_t board_frequency(void) {
  static uint64_t frequency;
  if (!frequency) {
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    frequency = hz ? hz : 24000000;
  }
  return frequency;
}

static void board_delay_us(uint64_t microseconds) {
  const uint64_t ticks = board_frequency() / 1000000ull * microseconds;
  const uint64_t start = board_counter();
  while (board_counter() - start < ticks) __asm__ volatile("yield" ::: "memory");
}

static void board_sleep_ms(unsigned milliseconds) { board_delay_us(milliseconds * 1000ull); }

#include "../BootResetCore.h"
#include "../soc/QcomRpmh.h"
#include "QcomUsbBoardInit.h"

/* XNU's C-side hooks: the component already maps its windows with ml_io_map (kernel/QcomXnuUsbDebug.c). */


struct regs {
  volatile uint32_t *base;
  vm_offset_t va;
  vm_size_t bytes;
};

static int regs_map(struct regs *r, uint64_t pa, uint64_t bytes) {
  r->base = 0;
  r->va = 0;
  r->bytes = (vm_size_t)bytes;
  vm_offset_t va = ml_io_map((vm_offset_t)pa, (vm_size_t)bytes);
  if (!va) return 0;
  r->va = va;
  r->base = (volatile uint32_t *)va;
  return 1;
}

static uint32_t rd(const struct regs *r, unsigned offset) { return r->base[offset / 4]; }

static void wr(const struct regs *r, unsigned offset, uint32_t value) {
  r->base[offset / 4] = value;
  __asm__ volatile("dsb sy" ::: "memory");
}

static void upd(const struct regs *r, unsigned offset, uint32_t mask, uint32_t value) {
  wr(r, offset, (rd(r, offset) & ~mask) | value);
}

static int wait_for(const struct regs *r, unsigned offset, uint32_t mask, uint32_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    if ((rd(r, offset) & mask) == value) return 1;
    board_delay_us(10);
  }
  return 0;
}

static const unsigned kCoreBcr = 0x39000;
static const unsigned kCoreGdsc = 0x39004;
static const unsigned kCoreClocks[] = {0x39088, 0x39018, 0x39024, 0x39028, 0x3908c};

static struct regs gGcc;

/* rpmh-rsc.c extracts the major version at bits 23:16; the DRV-2 selection is the aperture, bits 15:8 the
 * minor version. This board's DRV-2 reports 0x00030200, i.e. 3.2, and the version-3 status offsets below are
 * the ones USB votes with. */
static int supportedRscVersion(uint32_t id) { return ((id >> 16) & 0xff) == 3; }

static int refClock(void) {
  /* The OEM tcsrcc-pineapple driver uses a normal MMIO regmap and clk_branch2_ops. An SCM request for the
   * same register answers -2 on this SEC firmware (their 2026-11-15 readback), so MMIO is the working path. */
  const uintptr_t tcsrccBase = 0x01f40000;
  const unsigned usb2ClkrefOffset = 0x000b1118;
  const uintptr_t pa = tcsrccBase + usb2ClkrefOffset;
  const uintptr_t page = pa & ~(uintptr_t)0xfff;
  const unsigned reg = (unsigned)(pa - page);
  const uint32_t enable = 1u;
  struct regs tcsr;
  if (pa != 0x01ff1118ull) return 0; /* the static_assert of the upstream source, in C */
  if (!regs_map(&tcsr, page, 0x1000)) {
    IOLog("[usb] uke TCSR clkref pa=%08x map failed\n", (uint32_t)pa);
    return 0;
  }
  uint32_t before = rd(&tcsr, reg);
  if (!(before & enable)) {
    wr(&tcsr, reg, before | enable);
    board_delay_us(10); /* BRANCH_HALT_DELAY */
  }
  uint32_t after = rd(&tcsr, reg);
  IOLog("[usb] uke TCSR clkref pa=%08x before=%08x after=%08x\n", (uint32_t)pa, before, after);
  return (after & enable) != 0;
}

static int voteRails(int superSpeed) {
  struct regs db, rsc;
  const unsigned kDbSize = 0x20000, kDrvOffset = 0xd00, kUsbTcs = 2;
  if (!regs_map(&db, 0x81c60000ull, kDbSize) || !regs_map(&rsc, 0x17a20000ull, 0x10000)) return 0;
  /* The command DB is read in place instead of being copied into RAM: the window is uncached, so the
   * lookup is slower than upstream's snapshot, but the copy would be a 128 KiB static (this kernel's
   * __DATA has 16 KiB spare, STATE 101) or an allocation on the boot path. */
  const uint8_t *copy = (const uint8_t *)db.base;
  const uint32_t rscId = rd(&rsc, 0);
  QcomRscLayout layout = qcom_rsc_layout(rscId);
  const unsigned tcs = kDrvOffset + kUsbTcs * layout.tcs_stride;
  const unsigned kV3TcsStatus = 0x28, kV3CmdStatus = 0x40;
  const uint32_t kTriggered = 1u << 24, kIssuedAndComplete = (1u << 8) | (1u << 16);
  int ok = supportedRscVersion(rscId);
  const struct {
    const char *name;
    unsigned mv;
    int superSpeed;
  } rails[] = {
      {"ldob2", 880, 0}, {"ldob4", 1200, 0}, {"ldob7", 1800, 0}, {"ldob17", 3075, 0}, {"ldob3", 880, 1},
  };
  for (unsigned i = 0; ok && i < sizeof(rails) / sizeof(*rails); ++i) {
    if (rails[i].superSpeed != superSpeed) continue;
    QcomRpmhResource rail;
    memset(&rail, 0, sizeof(rail));
    int lookup = qcom_cmd_db_find(copy, kDbSize, rails[i].name, &rail);
    if (lookup != 0) {
      IOLog("[usb] uke cmd-db resource=%s status=%d\n", rails[i].name, lookup);
      ok = 0;
      continue;
    }
    uint32_t addresses[3], values[3];
    addresses[0] = rail.address + 8;
    addresses[1] = rail.address;
    addresses[2] = rail.address + 4;
    values[0] = 7;
    values[1] = rails[i].mv;
    values[2] = 1;
    uint32_t status = rd(&rsc, tcs + kV3TcsStatus);
    uint32_t control = rd(&rsc, tcs + layout.control);
    if (!status || (control & kTriggered)) {
      IOLog("[usb] uke rpmh busy resource=%s tcs=%u status=%08x control=%08x\n", rails[i].name, kUsbTcs,
             status, control);
      ok = 0;
      continue;
    }
    QcomRpmhWrite writes[20];
    unsigned n = qcom_rpmh_active_request(&layout, kUsbTcs, addresses, values, 3, writes, 20);
    if (!n) {
      ok = 0;
      continue;
    }
    for (unsigned w = 0; w < n; ++w) wr(&rsc, kDrvOffset + writes[w].offset, writes[w].value);
    for (unsigned c = 0; c < 3; ++c) {
      unsigned offset = tcs + kV3CmdStatus + c * layout.cmd_stride;
      if (!wait_for(&rsc, offset, kIssuedAndComplete, kIssuedAndComplete, 5000)) {
        IOLog("[usb] uke rpmh timeout resource=%s cmd=%u address=%08x status=%08x tcs=%08x irq=%08x\n",
               rails[i].name, c, addresses[c], rd(&rsc, offset), rd(&rsc, tcs + kV3TcsStatus),
               rd(&rsc, kDrvOffset + layout.irq_status));
        ok = 0;
        break;
      }
    }
    if (!ok) continue;
    unsigned done = qcom_rpmh_finish(&layout, kUsbTcs, writes, 20);
    for (unsigned w = 0; w < done; ++w) wr(&rsc, kDrvOffset + writes[w].offset, writes[w].value);
  }
  /* The clock and bus votes the OEM's eusb2 node carries: xo.lvl = 3 and an SN2 bandwidth request of
   * 60 MB/s (full speed) or 2.5 GB/s (super speed), scaled by the level's unit. */
  QcomRpmhResource xo, sn2;
  memset(&xo, 0, sizeof(xo));
  memset(&sn2, 0, sizeof(sn2));
  if (ok && !qcom_cmd_db_find(copy, kDbSize, "xo.lvl", &xo) && !qcom_cmd_db_find(copy, kDbSize, "SN2", &sn2) &&
      sn2.level_count >= 3) {
    uint32_t unit = sn2.levels[0] | (uint32_t)sn2.levels[1] << 16;
    uint64_t peak = superSpeed ? 2500000 : 60000;
    uint64_t scaled = unit ? (((peak * sn2.levels[2] + 15) / 16) * 1000 + unit - 1) / unit : 0;
    uint32_t vote = scaled > 0x3fff ? 0x3fff : (uint32_t)scaled;
    uint32_t addresses[2], values[2];
    addresses[0] = xo.address;
    addresses[1] = sn2.address;
    values[0] = 3;
    values[1] = 1u << 30 | 1u << 29 | vote << 14 | vote;
    uint32_t status = rd(&rsc, tcs + kV3TcsStatus);
    uint32_t control = rd(&rsc, tcs + layout.control);
    QcomRpmhWrite writes[20];
    unsigned n = (!status || (control & kTriggered))
                     ? 0
                     : qcom_rpmh_active_request(&layout, kUsbTcs, addresses, values, 2, writes, 20);
    if (!n) {
      ok = 0;
    } else {
      for (unsigned w = 0; w < n; ++w) wr(&rsc, kDrvOffset + writes[w].offset, writes[w].value);
      for (unsigned c = 0; c < 2; ++c) {
        unsigned offset = tcs + kV3CmdStatus + c * layout.cmd_stride;
        if (!wait_for(&rsc, offset, kIssuedAndComplete, kIssuedAndComplete, 5000)) {
          ok = 0;
          break;
        }
      }
      if (ok) {
        unsigned done = qcom_rpmh_finish(&layout, kUsbTcs, writes, 20);
        for (unsigned w = 0; w < done; ++w) wr(&rsc, kDrvOffset + writes[w].offset, writes[w].value);
      }
    }
  } else {
    ok = 0;
  }
  IOLog("[usb] uke rails rsc=%08x complete=%u\n", rscId, ok);
  return ok;
}

static int resetController(void) {
  if (!gGcc.base) return 0;
  /* dwc3_msm_link_clk_reset: gate the link clocks, assert the external block reset, then restore clocks. */
  for (unsigned i = 0; i < sizeof(kCoreClocks) / sizeof(*kCoreClocks); ++i) upd(&gGcc, kCoreClocks[i], 1, 0);
  upd(&gGcc, kCoreBcr, 1, 1);
  int asserted = wait_for(&gGcc, kCoreBcr, 1, 1, 200);
  board_sleep_ms(1);
  upd(&gGcc, kCoreBcr, 1, 0);
  int released = wait_for(&gGcc, kCoreBcr, 1, 0, 200);
  board_delay_us(1);
  int clocked = 1;
  for (unsigned i = 0; i < sizeof(kCoreClocks) / sizeof(*kCoreClocks); ++i) {
    upd(&gGcc, kCoreClocks[i], 1, 1);
    clocked = wait_for(&gGcc, kCoreClocks[i], 0x80000001u, 1, 200) && clocked;
  }
  board_sleep_ms(10);
  IOLog("[usb] uke BCR assert=%u release=%u clocks=%u\n", asserted, released, clocked);
  return asserted && released && clocked;
}

/* PMIC arbiter: the eUSB2 repeater of PM7550BA SID 7 sits at 0xfd00, and its device-mode tuning is the OEM's. */
static int repeater(int host) {
  struct regs core, owner, observer, channel;
  if (!regs_map(&core, 0xc400000ull, 0x3000) || !regs_map(&owner, 0xc42d000ull, 0x4000)) return 0;
  struct br_arbiter arb;
  arb.core = core.base;
  arb.cnfg = owner.base;   /* the owner/cnfg window is BootResetCore.h's "cnfg" */
  arb.ee = 0;
  uint32_t version = rd(&core, 0);
  unsigned count = rd(&core, BR_FEATURES) & BR_FEATURES_APIDS_MASK;
  if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !count || count > 1024) return 0;
  unsigned apid = 0;
  while (apid < count) {
    if (((rd(&core, BR_APID_MAP + 4 * apid) >> 8) & 0xfff) == 0x7fd &&
        (rd(&owner, BR_CNFG_OWNER + 4 * apid) & 7) == arb.ee)
      break;
    ++apid;
  }
  if (apid == count || !regs_map(&observer, 0xc440000ull, BR_OBS_APID_STRIDE * count) ||
      !regs_map(&channel, 0xc500000ull, BR_CHNL_APID_STRIDE * count))
    return 0;
  uint8_t value = 0;
  if (br_read8(observer.base, &arb, (int)apid, 0x46, &value) != 0) return 0;
  if (br_write8(channel.base, (int)apid, 0x46, (uint8_t)(value | 0x80)) != 0) return 0;
  /* Device and host properties of the effective eusb2-repeater@fd00 node. */
  static const uint8_t deviceTuning[] = {0, 0x0c, 3, 7, 2, 3, 0, 4, 2, 0, 3, 2};
  static const uint8_t hostTuning[] = {0, 0x08, 5, 7, 2, 1, 0, 6, 2, 0, 3, 2};
  const uint8_t *tuning = host ? hostTuning : deviceTuning;
  for (unsigned i = 0; i < 12; ++i)
    if (br_write8(channel.base, (int)apid, (uint8_t)(0x50 + i), tuning[i]) != 0) return 0;
  /* Program the forced value before enabling its override. */
  static const uint8_t forceRegisters[] = {0xed, 0xe8};
  for (unsigned f = 0; f < 2; ++f) {
    uint8_t force = host ? 0x40 : 0;
    uint8_t reg = forceRegisters[f];
    if (br_read8(observer.base, &arb, (int)apid, reg, &value) != 0) return 0;
    if (br_write8(channel.base, (int)apid, reg, (uint8_t)((value & ~0x40u) | force)) != 0) return 0;
    if (br_read8(observer.base, &arb, (int)apid, reg, &value) != 0) return 0;
    if ((value & 0x40u) != force) return 0;
  }
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    if (br_read8(observer.base, &arb, (int)apid, 8, &value) != 0) return 0;
    if (value & 0x80) return 1;
    board_delay_us(100);
  }
  return 0;
}

int qcom_usb_board_init(void) {
  if (!regs_map(&gGcc, 0x100000ull, 0x53000)) return -1;
  if (!voteRails(0)) return -2;
  upd(&gGcc, kCoreGdsc, 1, 0);
  if (!wait_for(&gGcc, kCoreGdsc, 1u << 31, 1u << 31, 10000)) return -3;
  /* Core at the OEM's 200 MHz source, GPLL0/3. Preserve mock UTMI's firmware divisor. */
  upd(&gGcc, 0x39030, 0x71f, 0x105);
  upd(&gGcc, 0x3902c, 0, 1);
  if (!wait_for(&gGcc, 0x3902c, 1, 0, 200) || !refClock() || !resetController()) return -4;
  if (!repeater(0)) return -6;
  IOLog("[usb] uke board init ok (rails + core + clkref + reset + repeater)\n");
  return 0;
}
