/* SPDX-License-Identifier: MIT
 * Replay the vendor eUSB2 PHY sequence over a generic 32-bit MMIO pair.
 *
 * See QcomUsbPhy.h for why, and QcomUsbPhyInit.h for the provenance of every op.
 */
#include "QcomUsbPhy.h"
#include "QcomUsbPhyInit.h"

/* This PHY's register file is byte-wide and answers a 32-bit read with the same byte in
 * all four lanes (b223 firmware dump, 2026-11-15: 0x01010101, 0xc8c8c8c8, ...).  Every
 * value the vendor sequence uses fits in one byte, so:
 *   read  -> keep the low byte only,
 *   write -> replicate the byte into all four lanes, which makes the store independent
 *            of which lane the bridge happens to consume.
 * Both halves of that rule were measured, not assumed. */
#define QPHY_BYTE(value) ((QUSB_U32)((value) & 0xFFu))
#define QPHY_LANES(byte) ((QUSB_U32)(((byte) & 0xFFu) * 0x01010101u))

QPHY_RESULT_CODE
QcomUsbPhyProgram(const QPHY_IO *io, QPHY_RESULT *result)
{
  const uke_eusb2_op *op;

  if (io == 0 || result == 0 || io->read32 == 0 || io->write32 == 0 || io->delay_us == 0) {
    return QphyBadArgument;
  }

  for (op = uke_eusb2_phy_init_seq; op->kind != UKE_EUSB2_END; op++) {
    QUSB_U32 raw = 0;
    result->entries++;

    switch (op->kind) {
    case UKE_EUSB2_PLATFORM:
      /* Regulators, the ref clocks, the PHY block reset and the three PMIC repeater
       * calls belong to the firmware / to SPMI. Record them so no reader can think they
       * ran here. */
      result->skipped_platform++;
      if (op->arg < 32u) {
        result->platform_mask |= (QUSB_U32)1u << op->arg;
      }
      break;

    case UKE_EUSB2_DELAY_US:
      io->delay_us(io->cookie, op->arg);
      result->delay_total_us += op->arg;
      break;

    case UKE_EUSB2_REG_RMW: {
      QUSB_U32 byte, wanted;
      if (!io->read32(io->cookie, io->base + op->off, &raw)) {
        return QphyReadFailed;
      }
      byte = QPHY_BYTE(raw);
      /* reg = (reg & ~clr) | val, evaluated on the byte the bridge actually holds. */
      wanted = (byte & ~QPHY_BYTE(op->clr)) | QPHY_BYTE(op->val);
      if (!io->write32(io->cookie, io->base + op->off, QPHY_LANES(wanted))) {
        return QphyWriteFailed;
      }
      result->writes++;
      break;
    }

    case UKE_EUSB2_REG_WRITE:
      /* The DT param-override store: plain 32-bit write, no mask, no read-back. */
      if (!io->write32(io->cookie, io->base + op->off, QPHY_LANES(QPHY_BYTE(op->val)))) {
        return QphyWriteFailed;
      }
      result->writes++;
      break;

    case UKE_EUSB2_CHK: {
      QUSB_U32 mask = QPHY_BYTE(op->chk_mask);
      if (!io->read32(io->cookie, io->base + op->off, &raw)) {
        return QphyReadFailed;
      }
      result->checks++;
      if ((QPHY_BYTE(raw) & mask) != (QPHY_BYTE(op->val) & mask)) {
        if (result->check_failures == 0u) {
          result->first_failed_offset = op->off;
          result->first_failed_mask = mask;
        }
        result->check_failures++;
      }
      break;
    }

    default:
      /* Unknown kind: refuse rather than silently skip an op nobody has classified. */
      return QphyBadArgument;
    }
  }

  return QphyOkay;
}
