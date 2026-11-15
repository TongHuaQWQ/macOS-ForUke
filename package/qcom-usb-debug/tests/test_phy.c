/* SPDX-License-Identifier: MIT
 * Host-side tests for the vendor eUSB2 PHY replay (soc/QcomUsbPhy.c).
 *
 * The mock models what the b223 firmware dump measured on this board: a byte-wide register
 * file behind a 32-bit bus, where a read answers the same byte on all four lanes and a write
 * lands on the byte regardless of which lane carries it.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "soc/QcomUsbPhy.h"
#include "soc/QcomUsbPhyInit.h"

#define PHY_BASE 0x088E3000ULL
#define PHY_BYTES 0x154u

struct model {
  unsigned char reg[PHY_BYTES];
  unsigned int reads, writes, delays;
  unsigned int delay_us_total;
  int fail_read, fail_write;
  unsigned int fail_read_at, fail_write_at; /* 1-based call index, 0 = every call */
};

static int read32(void *cookie, QUSB_U64 address, QUSB_U32 *value)
{
  struct model *m = cookie;
  unsigned int off = (unsigned int)(address - PHY_BASE);
  assert(address >= PHY_BASE && address < PHY_BASE + PHY_BYTES);
  assert(!(address & 3U));
  if (m->fail_read && (m->fail_read_at == 0 || ++m->reads >= m->fail_read_at)) return 0;
  ++m->reads;
  /* All four lanes carry the same byte. */
  *value = (QUSB_U32)m->reg[off] * 0x01010101U;
  return 1;
}

static int write32(void *cookie, QUSB_U64 address, QUSB_U32 value)
{
  struct model *m = cookie;
  unsigned int off = (unsigned int)(address - PHY_BASE);
  assert(address >= PHY_BASE && address < PHY_BASE + PHY_BYTES);
  assert(!(address & 3U));
  if (m->fail_write && (m->fail_write_at == 0 || ++m->writes >= m->fail_write_at)) return 0;
  ++m->writes;
  /* A 32-bit store writes the byte lane 0 carries; the upper lanes are aliases. */
  m->reg[off] = (unsigned char)(value & 0xFFU);
  return 1;
}

static void delay_us(void *cookie, QUSB_U32 usec)
{
  struct model *m = cookie;
  assert(usec <= 10000U); /* the platform refuses anything longer */
  ++m->delays;
  m->delay_us_total += usec;
}

static QPHY_IO io_for(struct model *m)
{
  QPHY_IO io;
  memset(&io, 0, sizeof io);
  io.base = PHY_BASE;
  io.read32 = read32;
  io.write32 = write32;
  io.delay_us = delay_us;
  io.cookie = m;
  return io;
}

/* The five offsets the b223 dump showed at the wrong value, plus the two the table leaves
 * clear at the end. Values are the vendor's steady state (see NOW.md, "b223 实机读数"). */
static void expect_vendor_state(const struct model *m)
{
  assert(m->reg[0x5c] == 0x90); /* PHY_CFG_PLL_FB_DIV_7_0 */
  assert(m->reg[0x60] == 0x01); /* PHY_CFG_PLL_FB_DIV_11_8 */
  assert(m->reg[0xb8] == 0x01); /* USB_PHY_FSEL_SEL */
  assert(m->reg[0x64] == 0x17); /* HS_PHY_CTRL2: VBUS_DET_EXT_SEL, SUSPEND_N_SEL, ... */
  assert(m->reg[0x54] == 0x0b); /* COMMON0: PHY_ENABLE|RETENABLEN|SIDDQ_SEL, SIDDQ=0 */
  assert(m->reg[0x3c] == 0x01); /* UTMI_CTRL0: SLEEPM=1 */
  assert(m->reg[0x94] == 0x00); /* CMN_CTRL_OVERRIDE_EN back to 0 */
  assert(m->reg[0x50] == 0x00); /* UTMI_CTRL5: POR=0 */
  assert(m->reg[0x58] == 0x00); /* DT param-override-seq writes 0 here */
}

int main(void)
{
  struct model m;
  QPHY_RESULT r;
  QPHY_IO io;

  /* 1. A device that matches the b223 hand-off state: the sequence must repair it, and the
   *    read-back checks must all pass because each write takes effect immediately. */
  memset(&m, 0, sizeof m);
  m.reg[0x5c] = 0xc8;
  m.reg[0x60] = 0x00;
  m.reg[0xb8] = 0x00;
  m.reg[0x64] = 0x07;
  m.reg[0x54] = 0x09;
  memset(&r, 0, sizeof r);
  io = io_for(&m);
  assert(QcomUsbPhyProgram(&io, &r) == QphyOkay);
  expect_vendor_state(&m);
  assert(r.check_failures == 0U);
  assert(r.checks > 20U);
  assert(r.writes > 20U);
  assert(r.delay_total_us == 150U);
  assert(m.delays == 1U);
  /* Regulator, ref clock, PHY reset and the three PMIC repeater calls are the firmware's
   * and SPMI's job; they must be counted, never silently pretended. */
  assert(r.skipped_platform > 5U);
  assert(r.platform_mask & ((QUSB_U32)1U << UKE_EUSB2_PLATFORM_CALL_REPEATER_INIT));

  /* 2. Replaying again on a PHY that is already in the vendor state changes nothing. */
  memset(&r, 0, sizeof r);
  assert(QcomUsbPhyProgram(&io, &r) == QphyOkay);
  expect_vendor_state(&m);
  assert(r.check_failures == 0U);

  /* 3. A PHY whose stores never land is a transport failure, not a silent no-op. */
  {
    struct model stuck;
    memset(&stuck, 0, sizeof stuck);
    stuck.reg[0x5c] = 0xc8;
    stuck.fail_write = 1;
    stuck.fail_write_at = 0; /* every write fails */
    io = io_for(&stuck);
    memset(&r, 0, sizeof r);
    assert(QcomUsbPhyProgram(&io, &r) == QphyWriteFailed);
  }

  /* 4. Read and delay failures are transport errors, not silent no-ops. */
  memset(&m, 0, sizeof m);
  m.fail_read = 1;
  io = io_for(&m);
  memset(&r, 0, sizeof r);
  assert(QcomUsbPhyProgram(&io, &r) == QphyReadFailed);

  memset(&m, 0, sizeof m);
  io = io_for(&m);
  io.delay_us = NULL;
  memset(&r, 0, sizeof r);
  assert(QcomUsbPhyProgram(&io, &r) == QphyBadArgument);
  assert(QcomUsbPhyProgram(NULL, &r) == QphyBadArgument);
  assert(QcomUsbPhyProgram(&io, NULL) == QphyBadArgument);

  /* 5. The window is the vendor's own 0x154 bytes: nothing in the table may reach past it. */
  {
    const uke_eusb2_op *op;
    for (op = uke_eusb2_phy_init_seq; op->kind != UKE_EUSB2_END; op++) {
      if (op->kind == UKE_EUSB2_REG_RMW || op->kind == UKE_EUSB2_REG_WRITE ||
          op->kind == UKE_EUSB2_CHK) {
        assert(op->off + 4U <= UKE_EUSB2_PHY_WINDOW);
        /* The datum always fits a byte (the bridge only carries one). The DT override store
         * carries an all-ones clr because it has no mask semantics at all. */
        assert(op->val <= 0xFFU && op->chk_mask <= 0xFFU);
        if (op->kind != UKE_EUSB2_REG_WRITE) assert(op->clr <= 0xFFU);
      }
    }
  }

  printf("phy: vendor eUSB2 sequence replay, byte-lane RMW and transport failures\n");
  return 0;
}
