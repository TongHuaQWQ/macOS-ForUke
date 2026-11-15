/* SPDX-License-Identifier: MIT
 * The vendor eUSB2 PHY register sequence, replayed from reverse-engineered data.
 *
 * platform: uke (SM7675). The table in QcomUsbPhyInit.h is the vendor's
 * msm_eusb2_phy_init() 19.2 MHz branch (phy-msm-snps-eusb2.ko), 29 read-modify-write
 * ops plus their read-back checks; every offset, mask and value came out of an
 * instruction, see work/re/docs/2026-11-14-eusb2-phy-vendor-sequence.md.
 *
 * Why this exists. The b223 firmware dump (2026-11-15) read the whole PHY window at
 * hand-off and compared it with that table: the PLL feedback dividers 0x5c/0x60 held
 * 0xc8/0x00 instead of 0x90/0x01, FSEL_SEL (0xb8) was clear, and USB2_SUSPEND/
 * VBUS_DET_EXT (0x64) were in the wrong state. A high-speed PLL that is fed the wrong
 * multiplier cannot chirp at 480 MHz, so the host only ever saw a full-speed device and
 * every control transfer ended in an EPIPE (-32). Running the vendor sequence is the
 * only step that is missing on this board: the firmware proves the clocks, GDSC, reset
 * and core state are already what the qualifier demands, and it also proves EUD is
 * clear (EUD would make the vendor driver skip this sequence entirely).
 *
 * What this cannot do: the regulator, clock and reset steps of the vendor sequence are
 * the firmware's job here (there is no regulator/clock framework in this kernel), and
 * the PMIC eUSB2 repeater powerup/reset/init calls are SPMI traffic this kernel has no
 * transport for. They are counted in the result so a log can never imply they ran.
 */
#ifndef QCOM_USB_PHY_H
#define QCOM_USB_PHY_H

#include "QcomUsbQualify.h" /* QUSB_U32/QUSB_U64 and the MMIO callback signatures */

typedef enum {
  QphyOkay = 0,
  QphyBadArgument = -1,
  QphyReadFailed = -2,
  QphyWriteFailed = -3,
  QphyDelayUnavailable = -4
} QPHY_RESULT_CODE;

typedef struct {
  /* PHY window base. Offsets from QcomUsbPhyInit.h are added to it, so the callbacks
   * see absolute addresses, exactly like QcomUsbQualify's. */
  QUSB_U64 base;
  QUSB_READ32 read32;
  QUSB_WRITE32 write32;
  QUSB_DELAY_US delay_us; /* required: the sequence has one 150 us step */
  void *cookie;
} QPHY_IO;

typedef struct {
  QUSB_U32 entries;          /* table entries walked */
  QUSB_U32 writes;           /* REG_RMW + REG_WRITE stores issued */
  QUSB_U32 checks;           /* read-back checks run */
  QUSB_U32 check_failures;   /* checks whose masked value did not match */
  QUSB_U32 delay_total_us;
  QUSB_U32 skipped_platform; /* regulator/clock/reset/repeater steps not done here */
  QUSB_U32 platform_mask;    /* bit i set = UKE_EUSB2_PLATFORM_* id i was skipped */
  QUSB_U32 first_failed_offset;
  QUSB_U32 first_failed_mask;
} QPHY_RESULT;

/* Replay the table. Returns QphyOkay when every store and read completed; read-back
 * mismatches do not fail the call, because the vendor driver only pr_err()s them and a
 * partially initialised PHY that gets the core started is still worth having. */
QPHY_RESULT_CODE QcomUsbPhyProgram(const QPHY_IO *io, QPHY_RESULT *result);

#endif /* QCOM_USB_PHY_H */
