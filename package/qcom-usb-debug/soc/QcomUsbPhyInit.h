/* AUTO-GENERATED from work/re/data/eusb2-phy-init-19mhz.json — do not hand-edit.
 * Source: vendor phy-msm-snps-eusb2.ko, msm_eusb2_phy_init() 19.2 MHz branch.
 * Device: Xiaomi Pad 7 (uke / SM7675 cliffs), eUSB2 PHY @ 0x88e3000, window 0x154 bytes.
 * Every offset/value/mask below was read out of an instruction; see the .md for evidence.  * Copy of work/re/include/eusb2-phy-init-uke.h. Regenerate with
 * work/re/tools/gen_eusb2_header.py from work/re/data/eusb2-phy-init-19mhz.json; do not hand-edit.
 *
 * Byte-lane note (2026-11-15, b223 device dump): this PHY answers a 32-bit read with the SAME
 * byte in all four lanes (0x01010101, 0xc8c8c8c8, ...), and every value in this table is <= 0xff.
 * soc/QcomUsbPhy.c therefore reads the low byte and writes the byte replicated into all four
 * lanes, which is what makes the vendor's read-modify-write semantics work over this bridge.
 */

#ifndef UKE_EUSB2_PHY_INIT_H
#define UKE_EUSB2_PHY_INIT_H

#define UKE_EUSB2_REF_CLK_HZ   19200000u   /* clk_get_rate(ref_clk_src) branch 0x124f800 */
#define UKE_EUSB2_PHY_WINDOW   0x154u

typedef enum {
  UKE_EUSB2_END = 0,
  UKE_EUSB2_REG_RMW,      /* reg = (reg & ~clr) | val ; then read back, (reg & chk_mask) == chk_val */
  UKE_EUSB2_REG_WRITE,    /* plain 32-bit store, NO mask, NO read-back (DT override-seq semantics) */
  UKE_EUSB2_CHK,          /* read back only (the vendor only pr_err()s, never fails) */
  UKE_EUSB2_DELAY_US,
  UKE_EUSB2_PLATFORM,     /* regulator / clock / reset step: see step name */
} uke_eusb2_op_kind;

typedef struct {
  unsigned short kind;    /* uke_eusb2_op_kind */
  unsigned short off;     /* PHY window offset (0x000..0x153) or 0 for non-register steps */
  unsigned int   clr;     /* bits to clear before OR-ing val (RMW) */
  unsigned int   val;     /* value OR-ed in (RMW) / expected value (CHK) */
  unsigned int   chk_mask;/* mask for the read-back compare */
  unsigned int   arg;     /* delay us, or a UKE_EUSB2_PLATFORM_* id */
  const char    *step;    /* platform step name, else 0 */
} uke_eusb2_op;

/* platform steps the driver must do with OS APIs (not MMIO) */
enum {
  UKE_EUSB2_PLATFORM_REGULATOR_SET_LOAD_VDD,
  UKE_EUSB2_PLATFORM_REGULATOR_SET_VOLTAGE_VDD,
  UKE_EUSB2_PLATFORM_REGULATOR_ENABLE_VDD,
  UKE_EUSB2_PLATFORM_REGULATOR_SET_LOAD_VDDA12,
  UKE_EUSB2_PLATFORM_REGULATOR_SET_VOLTAGE_VDDA12,
  UKE_EUSB2_PLATFORM_REGULATOR_ENABLE_VDDA12,
  UKE_EUSB2_PLATFORM_BARRIER_DSB_SY,
  UKE_EUSB2_PLATFORM_CLK_PREPARE_REF_CLK_SRC,
  UKE_EUSB2_PLATFORM_CLK_ENABLE_REF_CLK_SRC,
  UKE_EUSB2_PLATFORM_CLK_PREPARE_REF_CLK,
  UKE_EUSB2_PLATFORM_CLK_ENABLE_REF_CLK,
  UKE_EUSB2_PLATFORM_RESET_ASSERT_PHY_RESET,
  UKE_EUSB2_PLATFORM_RESET_DEASSERT_PHY_RESET,
  UKE_EUSB2_PLATFORM_CLK_RATE_READ_REF_CLK_SRC,
  UKE_EUSB2_PLATFORM_EUD_DETECT_WRITE,   /* writel(1) to the 4-byte window @0x0c278000 */
  UKE_EUSB2_PLATFORM_CALL_REPEATER_POWERUP,
  UKE_EUSB2_PLATFORM_CALL_REPEATER_RESET,
  UKE_EUSB2_PLATFORM_CALL_REPEATER_INIT,
};

static const uke_eusb2_op uke_eusb2_phy_init_seq[] = {
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_REGULATOR_SET_LOAD_VDD, "regulator_set_load vdd" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_REGULATOR_SET_VOLTAGE_VDD, "regulator_set_voltage vdd" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_REGULATOR_ENABLE_VDD, "regulator_enable vdd" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_REGULATOR_SET_LOAD_VDDA12, "regulator_set_load vdda12" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_REGULATOR_SET_VOLTAGE_VDDA12, "regulator_set_voltage vdda12" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_REGULATOR_ENABLE_VDDA12, "regulator_enable vdda12" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_BARRIER_DSB_SY, "mb " },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0x1, 0u, UKE_EUSB2_PLATFORM_EUD_DETECT_WRITE, "eud_detect_reg" },   /* separate window @0x0c278000 */
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CALL_REPEATER_POWERUP, "usb_repeater_powerup" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CALL_REPEATER_RESET, "usb_repeater_reset" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CALL_REPEATER_INIT, "usb_repeater_init" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CLK_PREPARE_REF_CLK_SRC, "clk_prepare ref_clk_src" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CLK_ENABLE_REF_CLK_SRC, "clk_enable ref_clk_src" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CLK_PREPARE_REF_CLK, "clk_prepare ref_clk" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CLK_ENABLE_REF_CLK, "clk_enable ref_clk" },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_RESET_ASSERT_PHY_RESET, "reset_assert " },
  { UKE_EUSB2_DELAY_US, 0u, 0u, 0u, 0u, 150u, 0 },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_RESET_DEASSERT_PHY_RESET, "reset_deassert " },
  { UKE_EUSB2_REG_RMW, 0x94, 0x2, 0x2, 0x2, 0, 0 },   /* USB_PHY_CFG0: CMN_CTRL_OVERRIDE_EN=1 */
  { UKE_EUSB2_CHK, 0x94, 0u, 0x2, 0x2, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x50, 0x2, 0x2, 0x2, 0, 0 },   /* USB_PHY_UTMI_CTRL5: POR=1 */
  { UKE_EUSB2_CHK, 0x50, 0u, 0x2, 0x2, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x54, 0x9, 0x9, 0x9, 0, 0 },   /* USB_PHY_HS_PHY_CTRL_COMMON0: PHY_ENABLE=1, RETENABLEN=1 */
  { UKE_EUSB2_CHK, 0x54, 0u, 0x9, 0x9, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x130, 0x4, 0x4, 0x4, 0, 0 },   /* USB_PHY_APB_ACCESS_CMD: APB_LOGIC_RESET=1 */
  { UKE_EUSB2_CHK, 0x130, 0u, 0x4, 0x4, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x98, 0x40, 0x0, 0x40, 0, 0 },   /* UTMI_PHY_CMN_CTRL0: TESTBURNIN=0 */
  { UKE_EUSB2_CHK, 0x98, 0u, 0x0, 0x40, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0xb8, 0x1, 0x1, 0x1, 0, 0 },   /* USB_PHY_FSEL_SEL: FSEL_SEL=1 */
  { UKE_EUSB2_CHK, 0xb8, 0u, 0x1, 0x1, 0, 0 },
  { UKE_EUSB2_PLATFORM, 0u, 0u, 0u, 0u, UKE_EUSB2_PLATFORM_CLK_RATE_READ_REF_CLK_SRC, "clk_rate_read ref_clk_src" },
  { UKE_EUSB2_REG_RMW, 0x54, 0x70, 0x0, 0x70, 0, 0 },   /* FSEL = 0 (19.2 MHz) */
  { UKE_EUSB2_CHK, 0x54, 0u, 0x0, 0x70, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x5c, 0xff, 0x90, 0xff, 0, 0 },   /* PHY_CFG_PLL_FB_DIV_7_0 = 0x90 */
  { UKE_EUSB2_CHK, 0x5c, 0u, 0x90, 0xff, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x60, 0xf, 0x1, 0xf, 0, 0 },   /* PHY_CFG_PLL_FB_DIV_11_8 = 1 */
  { UKE_EUSB2_CHK, 0x60, 0u, 0x1, 0xf, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x60, 0xf0, 0x0, 0xf0, 0, 0 },   /* PHY_CFG_PLL_REF_DIV = 0 */
  { UKE_EUSB2_CHK, 0x60, 0u, 0x0, 0xf0, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x58, 0xfe, 0x2, 0xfe, 0, 0 },   /* PHY_CFG_PLL_CPBIAS_CNTRL = 1<<1 */
  { UKE_EUSB2_CHK, 0x58, 0u, 0x2, 0xfe, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x68, 0xfc, 0x20, 0xfc, 0, 0 },   /* PHY_CFG_PLL_INT_CNTRL = 8<<2 */
  { UKE_EUSB2_CHK, 0x68, 0u, 0x20, 0xfc, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x68, 0x3, 0x1, 0x3, 0, 0 },   /* PHY_CFG_PLL_GMP_CNTRL = 1<<0 */
  { UKE_EUSB2_CHK, 0x68, 0u, 0x1, 0x3, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x6c, 0x1f, 0x10, 0x1f, 0, 0 },   /* PHY_CFG_PLL_PROP_CNTRL = 0x10<<0 */
  { UKE_EUSB2_CHK, 0x6c, 0u, 0x10, 0x1f, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x70, 0x7, 0x0, 0x7, 0, 0 },   /* PHY_CFG_PLL_VCO_CNTRL = 0 */
  { UKE_EUSB2_CHK, 0x70, 0u, 0x0, 0x7, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x6c, 0xc0, 0x40, 0xc0, 0, 0 },   /* PHY_CFG_PLL_VREF_TUNE = 1<<6 */
  { UKE_EUSB2_CHK, 0x6c, 0u, 0x40, 0xc0, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x64, 0x10, 0x10, 0x10, 0, 0 },   /* USB_PHY_HS_PHY_CTRL2: VBUS_DET_EXT_SEL=1 */
  { UKE_EUSB2_CHK, 0x64, 0u, 0x10, 0x10, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x7c, 0x7, 0x0, 0x7, 0, 0 },   /* PHY_CFG_TX_PREEMP_TUNE = 0 */
  { UKE_EUSB2_CHK, 0x7c, 0u, 0x0, 0x7, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x7c, 0x60, 0x40, 0x60, 0, 0 },   /* PHY_CFG_TX_RISE_TUNE = 2<<5 */
  { UKE_EUSB2_CHK, 0x7c, 0u, 0x40, 0x60, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x7c, 0x18, 0x8, 0x18, 0, 0 },   /* PHY_CFG_TX_RES_TUNE = 1<<3 */
  { UKE_EUSB2_CHK, 0x7c, 0u, 0x8, 0x18, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x78, 0x38, 0x18, 0x38, 0, 0 },   /* PHY_CFG_TX_HS_VREF_TUNE = 3<<3 */
  { UKE_EUSB2_CHK, 0x78, 0u, 0x18, 0x38, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x78, 0xc0, 0x0, 0xc0, 0, 0 },   /* PHY_CFG_TX_HS_XV_TUNE = 0 */
  { UKE_EUSB2_CHK, 0x78, 0u, 0x0, 0xc0, 0, 0 },
  { UKE_EUSB2_REG_WRITE, 0x58, 0xffffffffu, 0x00, 0u, 0, "qcom,param-override-seq" },   /* unmasked: clobbers whatever the masked write above set */
  /* SKIPPED BY DEFAULT (debugfs field, 0xff sentinel): tx_pre_emphasis cond=1 <= tx_pre_emphasis <= 7 off=0x7c */
  /* runtime-variable check skipped: off 0x7c expects (tx_pre_emphasis << 0) & 0x7 */
  /* SKIPPED BY DEFAULT (debugfs field, 0xff sentinel): tx_rise_fall_time cond=1 <= tx_rise_fall_time <= 4 off=0x7c */
  /* runtime-variable check skipped: off 0x7c expects (tx_rise_fall_time << 5) & 0x60 */
  /* SKIPPED BY DEFAULT (debugfs field, 0xff sentinel): tx_src_impedence cond=1 <= tx_src_impedence <= 4 off=0x7c */
  /* runtime-variable check skipped: off 0x7c expects (tx_src_impedence << 3) & 0x18 */
  /* SKIPPED BY DEFAULT (debugfs field, 0xff sentinel): tx_dc_vref cond=1 <= tx_dc_vref <= 7 off=0x78 */
  /* runtime-variable check skipped: off 0x78 expects (tx_dc_vref << 3) & 0x38 */
  /* SKIPPED BY DEFAULT (debugfs field, 0xff sentinel): tx_xv cond=1 <= tx_xv <= 4 off=0x78 */
  /* runtime-variable check skipped: off 0x78 expects (tx_xv << 6) & 0xc0 */
  { UKE_EUSB2_REG_RMW, 0x64, 0xc, 0xc, 0xc, 0, 0 },   /* USB_PHY_HS_PHY_CTRL2: USB2_SUSPEND_N_SEL=1, USB2_SUSPEND_N=1 */
  { UKE_EUSB2_CHK, 0x64, 0u, 0xc, 0xc, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x3c, 0x1, 0x1, 0x1, 0, 0 },   /* USB_PHY_UTMI_CTRL0: SLEEPM=1 */
  { UKE_EUSB2_CHK, 0x3c, 0u, 0x1, 0x1, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x54, 0x2, 0x2, 0x2, 0, 0 },   /* USB_PHY_HS_PHY_CTRL_COMMON0: SIDDQ_SEL=1 */
  { UKE_EUSB2_CHK, 0x54, 0u, 0x2, 0x2, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x54, 0x4, 0x0, 0x4, 0, 0 },   /* USB_PHY_HS_PHY_CTRL_COMMON0: SIDDQ=0 */
  { UKE_EUSB2_CHK, 0x54, 0u, 0x0, 0x4, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x50, 0x2, 0x0, 0x2, 0, 0 },   /* USB_PHY_UTMI_CTRL5: POR=0 */
  { UKE_EUSB2_CHK, 0x50, 0u, 0x0, 0x2, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x64, 0x8, 0x0, 0x8, 0, 0 },   /* USB_PHY_HS_PHY_CTRL2: USB2_SUSPEND_N_SEL=0 */
  { UKE_EUSB2_CHK, 0x64, 0u, 0x0, 0x8, 0, 0 },
  { UKE_EUSB2_REG_RMW, 0x94, 0x2, 0x0, 0x2, 0, 0 },   /* USB_PHY_CFG0: CMN_CTRL_OVERRIDE_EN=0 */
  { UKE_EUSB2_CHK, 0x94, 0u, 0x0, 0x2, 0, 0 },
  { UKE_EUSB2_END, 0u, 0u, 0u, 0u, 0u, 0 }
};

#endif /* UKE_EUSB2_PHY_INIT_H */
