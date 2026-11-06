/* SPDX-License-Identifier: MIT
 * Cliffs (SM7675) GCC + QSMMUv500 + DWC3 profile for QcomXnuUsbDebug.
 *
 * Target: Xiaomi Pad 7 "uke", SM7675, 8 GiB, 4 KiB pages.
 *
 * Register layout assumed = the SM8550 profile's layout: Qualcomm QSCRATCH wrapper, clk_branch2
 * CBCRs, one GDSC with qcom,support-cfg-gdscr, non-secure QSMMUv500 (SMMUv2 programming model),
 * inherited ABL DEVICE-mode PHY and power. Only the resource values below differ.
 *
 * Every value names its source. The board's own live device tree
 * (device/dt/live-runtime.dtb, dumped to analysis/ref/dt/live.dts) is the first source; the
 * cliffs register layout comes from the SM8650/vendor GCC driver
 * (oppo-source/android_kernel_oppo_sm8650 drivers/clk/qcom/gcc-cliffs.c), which our DT shares
 * through compatible = "qcom,cliffs-gcc".
 */
#ifndef QUD_UKE_H
#define QUD_UKE_H
#define QUD_SOC_NAME "sm7675-inherited"

/* Development IDs inherited from m1n1 and from the SM8550/SM8250 profiles; not a product
 * allocation. Kept identical so one host collector finds every port. */
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"

/* GCC: live.dts:13381  gcc: clock-controller@100000, compatible "qcom,cliffs-gcc",
 * reg <0x100000 0x1f4200>. The 0x1f4200 covers the 0x390xx USB3-PRIM window and the
 * 0x139xxx GDSC mirrors in one mapping. */
#define QUD_GCC_BASE 0x00100000ULL
#define QUD_GCC_BYTES 0x1f4200u

/* DWC3 core: live.dts:15967-15968  usb0: ssusb@a600000, compatible "qcom,dwc-usb3-msm",
 * reg <0xa600000 0x100000>; the QSCRATCH wrapper is inside that window at +0xf8800. */
#define QUD_DWC3_BASE 0x0a600000ULL
#define QUD_DWC3_BYTES 0x100000u

/* Apps SMMU: live.dts:15265  apps_smmu: apps-smmu@15000000, compatible "qcom,qsmmu-v500",
 * reg <0x15000000 0x100000 0x151fe000 0x40>. */
#define QUD_SMMU_BASE 0x15000000ULL
#define QUD_SMMU_BYTES 0x100000ULL

/* USB stream ID: live.dts:15998-16001  dwc3@a600000 iommus = <0x129 0x40 0x00>, i.e. apps_smmu
 * stream 0x40. (SM8250 is 0x0, Kalama 0x40.) */
#define QUD_STREAM_ID 0x40u

/* Device-visible IOVA for the module's own DMA arena: live.dts:16000
 * qcom,iommu-dma-addr-pool = <0x90000000 0x60000000>. 2 MiB aligned, 32-bit, inside the pool the
 * firmware itself uses. The installer scans the stream table before writing, so a collision with a
 * live stream is refused rather than silently aliased. */
#define QUD_DMA_IOVA 0x90000000ULL

/* The five CBCRs inspected before the DWC3 is touched.
 * Source: gcc-cliffs.c clk_branch halt_reg / enable_reg:
 *   gcc_usb30_prim_master_clk          0x39018  BRANCH_HALT
 *   gcc_usb30_prim_sleep_clk           0x39024  BRANCH_HALT
 *   gcc_usb30_prim_mock_utmi_clk       0x39028  BRANCH_HALT
 *   gcc_usb3_prim_phy_aux_clk          0x39060  BRANCH_HALT
 *   gcc_usb3_prim_phy_com_aux_clk      0x39064  BRANCH_HALT
 * Enable bit is BIT(0), CLK_OFF is BIT(31) (clk-branch.c). The two AXI branches
 * (gcc_cfg_noc_usb3_prim_axi_clk 0x39088, gcc_aggre_usb3_prim_axi_clk 0x3908c) are not in the
 * inspected set: the module checks the gadget's own clocks, and the AXI votes belong to the
 * interconnect path the firmware already established. */
#define QUD_GCC_CBCR_OFFSETS {0x39018u, 0x39024u, 0x39028u, 0x39060u, 0x39064u}

/* This board's usb_phy nodes describe an eUSB2 repeater (qcom,use-eusb2-phy,
 * live.dts:15983) and no SuperSpeed PHY, so the two PHY auxiliary branches may legitimately read
 * halted while the controller runs at high speed. The first three are what a high-speed gadget
 * needs and stay required. Same reasoning as the SM8250 profile. */
#define QUD_GCC_CBCR_REQUIRED 3u

/* Block reset for the primary USB3 wrapper. Not visible in the live DT (it names only the reset
 * ID, live.dts:15977 resets = <0x16e 0x13>); taken from the layout the GDSCR below proves:
 * the GDSC sits at gdscr = bcr + 4 and cfg_gdscr = gdscr + 4 for this GCC generation. */
#define QUD_GCC_BCR_OFFSET 0x39000u

/* usb30_prim_gdsc: live.dts:15764  qcom,gdsc@139004 reg <0x139004 0x04> is an *absolute*
 * address, so the GCC-relative offset is 0x139004 - 0x100000 = 0x39004. */
#define QUD_GCC_GDSCR_OFFSET 0x39004u

/* CFG_GDSCR = GDSCR + 0x4, and this GDSC carries qcom,support-cfg-gdscr (live.dts:15769), so the
 * power state is CFG_GDSCR.POWER_UP_COMPLETE BIT(16) rather than the legacy GDSCR.PWR_ON. That is
 * why QUD_GDSC_POWER_BIT_LEGACY stays undefined here. */
#define QUD_GCC_CFG_GDSCR_OFFSET 0x39008u

/* QSCRATCH inside the wrapper window: wrapper base 0xa6f8800 (live.dts:15967 window + 0xf8800)
 * plus QSCRATCH_HS_PHY_CTRL 0x10 (dwc3-qcom.c) = 0xa6f8810, i.e. 0xf8810 relative to
 * QUD_DWC3_BASE. Bit meanings: UTMI_OTG_VBUS_VALID BIT(20), SW_SESSVLD_SEL BIT(28). */
#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0xf8810u
#define QUD_QSCRATCH_VBUS_MASK ((1u << 20) | (1u << 28))

/* PSCI SYSTEM_RESET through SMC: NOT yet qualified on this board. ArmDTPlatform::platformStageReset()
 * issues SMC 0x84000009 for the sheng-platform-stage probes, but this port has never observed one of
 * those probes actually reboot the tablet, so per this module's rule the profile leaves the reset
 * callback out. Logging and the status/screenshot channel do not depend on it; flip this to 1 after a
 * stage probe is seen resetting. */
#define QUD_PSCI_SMC_RESET 0

/* Screenshot geometry is not described by any source in this workspace; the module's defaults
 * (from include/QcomUsbScreenshot.h) apply. The board's scanout is the MDP/DPU framebuffer the
 * legacy display path hands over, not a fixed CPU mapping. */
#endif
