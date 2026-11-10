# macOS-ForUke

Device repository for the Xiaomi Pad 7 / POCO Pad X1 (`uke`, SM7675 "Cliffs", Adreno 732), running
**macOS 26.5.2 (build 25F84)** through UEFI with a self-built XNU.

Format 2 device repository: the board pins the SM7675 SoC layer in `feeds.toml`, the OS identity lives in
the shared profile `macos-26.5.2-25F84`, and `porting.toml` records the state of each hardware area.
`AGENTS.md` has the editing rules; `docs/STATE.md` is the handoff record and `docs/PROGRESS.md` the
history behind it.

## Where the port is

Three layers, three repositories:

| Layer | Repository | Produces |
| --- | --- | --- |
| core | `TongHuaQWQ/UniXNU` (fork of UniXNU, branch `macos`) | `./unixnu`: the kernel build, the prelink, the payload, the boot image |
| device | this one | `board.toml`, `configs/`, `patch` series, kernel-side packages, CI workflows |
| firmware | `uefi-uke` (Mu-Silicium fork, `Platforms/Xiaomi/ukePkg`) | `boot.img` — UEFI, UniXNU menu, DarwinBoot |

Proven on hardware:

- the whole boot chain: fastboot → ABL → UEFI → `UniXNU.efi` → DarwinBoot → XNU;
- the kernel is a real `MH_FILESET` built with the collection's own kexts paired against Apple's
  kernelcache — **0 stubs**: `stubbed {}`, `stub_sites {}`, `functions 17953/23025`,
  `kext-facing targets named 3470/3826`, `unresolved closure references 109` (2026-11-09, `0190eb6`);
- the kernel's own log survives a reset: `UkeLastLog` appends 16 KiB `UKELLOG1` records to the last
  32 MiB of userdata every 2 s, read back with `dd` in TWRP (`--self-test` writes a 4 KiB block,
  reads it back byte-identical and clears it — verified on the tablet);
- the panel is the other log channel: `package/qcom-usb-debug` paints a QR card carrying a 480-byte
  diagnostic payload (state line, setup packet, traces, request history).

Current blocker — **USB enumeration** (the kernel itself reaches the driver that needs it):

```
b108 panel QR, decoded:
  PH=15 E=11 CR=-3 S=0 ER=0 LS=0      PH 15 = CORE_INIT, E 11 = CORE_INIT_FAILED
  T2 c704 40f00000                    trace TIMEOUT, reg 0xc704 = DCTL, bit 30 = CSftRst still set
  T1 33313130                         trace ID, GSNPSID 0x33313130 = DWC_usb31
```

The core is identified and halts correctly, then never finishes its soft reset inside the 10 ms the
driver allowed it. `096a8e7` gives that reset 100 ms (what Linux allows), which is the first thing to
test on hardware — nothing about it is verified yet.

Open, tracked in `docs/STATE.md`: whether the XNU-side DWC3 takeover should inherit a live vendor USB
stack from the firmware (`UKE_VENDOR_USB`, currently 0 because the vendor chain does not survive this
board), and the tail of the log window overlapping foreign filesystem metadata (518 bytes at the window's
last 16 KiB slot), so the window has to shrink before it can be written for a full hour.

## The loop this repository is built around

A kernel is ~30 minutes of CI (XNU is compiled and prelinked on a macOS runner; the device side is
Linux). One command turns a finished run into flashable images:

```sh
python3 work/round.py <run-id> --name bNN-<tag>          # fetch artifact, build payload + images
python3 work/round.py --kernel <kernel.macho> --name …  # rebuild the images only
```

It downloads the `uke-kernel-min` artifact, rebuilds the payload from that kernel with the local UEFI
build and the device tree, copies both images into `work/flash/`, records their sizes and sha256 in
`work/flash/INDEX.md`, and prints the flash commands.

CI: pushing a `ci-*` branch runs `.github/workflows/build-kernel-min.yml` with the round's defaults
(`uke-macos_defconfig`, core `TongHuaQWQ/UniXNU@macos`). `workflow_dispatch` needs a token scope this
setup does not have, so a branch push is the trigger that works.

## Build, flash, read

```sh
# 1. flash the kernel round (boot_a is the firmware, vendor_boot_a is the kernel + payload)
fastboot --set-active=a
fastboot flash boot_a         work/flash/boot-round-<name>.img
fastboot flash vendor_boot_a  work/flash/vendor_boot_round-<name>.img
fastboot reboot

# 2. let it run for at least 10 s (the log writer emits a record every 2 s), then power off

# 3. read it back: the stock images must come first, an XNU boot_a cannot reach recovery
fastboot flash boot_a         device/uke-backup/uke-backup/boot_a.img
fastboot flash vendor_boot_a  device/uke-backup/uke-backup/vendor_boot_a.img
fastboot reboot recovery
sleep 10; ADB_LIBUSB=0 adb start-server; adb devices -l

# 4. dump and decode the window
bash ../analysis/log-capture/read-ukelog-twrp.sh            # geometry, dd, decode
bash ../analysis/log-capture/read-ukelog-twrp.sh --self-test # prove the window itself first
```

`fastboot flash` always, never `fastboot boot`: this ABL answers
`Failed to load/authenticate boot image: Load Error`. Use `fastboot set_active a` before each test —
the ABL marks a slot unbootable after repeated non-Android boots.

## Diagnostics

| Channel | Read with | Survives |
| --- | --- | --- |
| panel QR card | `work/analysis/log-capture/panel_qr_decode.py` (photo or 4K recording; a 3200×2136 panel must not be downscaled below ~3800 px) | needs a camera |
| userdata window | `work/analysis/log-capture/read-ukelog-twrp.sh` → `tools/decode-ukelog.py` | reset, power cycle |
| pram (msgbuf snapshot, panic text, writer beacon) | the firmware menu on the next boot (`UkeBootMemoryDxe` scans for `panic(` and prints pram+0x1100) | warm reset only |
| msgbuf over USB | `package/qcom-usb-debug/scripts/collect-xnu-usb.py` | needs enumeration, which is the current blocker |

The writer reports its own progress into the pram beacon slot, so a boot whose window is empty still
names the stage: `UKELOG-TRANSPORT-OK`, `UKELOG-NOTRANSPORT`, `UKELOG-WINDOW-OPEN`, `UKELOG-NOWINDOW`.

## Configuration

`configs/uke-macos_defconfig` is the round's configuration: source corecrypto, pthread, mockfs,
`ArmDTPlatform`, `ArmCpu`, the USB diagnostic console, the log writer (`CONFIG_XNU_BOOT_LOG` +
`CONFIG_XNU_LOG_USERDATA`, force-loading `libUkeUfsLog.a` for the userdata half), and the fileset
reference kernel that the prelink compares against. `CONFIG_XNU_NATIVE_UFS`, `CONFIG_XNU_NATIVE_AES` and
`CONFIG_XNU_SHENG_PMGR` are off here; the header of that file says why, and it is not an oversight.

The other configurations are kept for the paths they cover: `uke_defconfig` (first console),
`uke-storage_defconfig`, `uke-userland_defconfig`, `uke-full_defconfig`, `uke-full-plus_defconfig`
(sheng parity, needs inputs CI cannot produce), `uke-gpu_defconfig`, `uke-nodisplay_defconfig`,
`bootstrap_defconfig`.

Kernel-side packages, linked as `kits/uke`:

- `package/UkeLastLog.c` — msgbuf → pram and msgbuf → userdata log writer;
- `package/UfsLog.cpp` + `package/UfsLogKernel.c` — the freestanding UFS transport behind that writer
  (the driver's request list, SCSI layer and SMMU takeover, without IOKit); `scripts/check-ufs-log.py`
  compiles and links both for `aarch64-none-elf` on the host in about a second;
- `package/qcom-usb-debug` — the DWC3 device-mode console and panel diagnostics;
- `package/CrashLogProbe.c`, `package/BootWatchdogPolicy.c`, `package/MissingKernelSymbols.c`.

## Hardware data (from the tablet)

- Device `2410CRP4CC`, 12 GB RAM, 256 GB UFS; 5 GiB of it high DRAM, 4 GiB owned by XNU.
- `userdata` = `/dev/block/sda32`, 58,988,539 × 4096 B (225 GiB), first block 2,961,408, last
  61,949,946. The log window is its last 8192 blocks: **61,941,755 … 61,949,946** (the same blocks TWRP
  reads at `skip=58980347`). `[userdata]` in `board.toml` predates this measurement and is stale.
- `splash_region` `0xe3940000` + `0x2b00000`; UFS host `0x1d84000`, stream `0x60`; DWC3 `0xa600000`,
  eUSB2 PHY `0x88e3000` + PMIC repeater `0xfd00` (no SuperSpeed PHY).
- Panel O82 `42_02_0b`, 3200×2136, selected by `msm_drm.dsi_display0=…o82_42_02_0b_dualdsi_dsc_vid:`;
  touch NT36532 on GENI SPI `0xa90000`, IRQ GPIO 54, lcd_id GPIO 26.
- Stock boot: Android header v4, 4096-byte pages.
- Kernel: 4 KiB pages, `xnu-12377.121.6` (`ac9718f`), profile `macos-26.5.2-25F84`, arm64e/PAC.

## Known gaps

- USB enumeration: see the blocker above. Nothing downstream of it (audio, Wi-Fi, sensors) is part of
  this pass.
- The log window overlaps foreign metadata at its last 16 KiB slot; it needs to shrink before a long
  boot can fill it.
- The persistent-data path (`sheng-data-persist`) and the APFS/launchd chain still need a device run
  with a working console.
- `UKE_VENDOR_HW` / `UKE_VENDOR_USB` stay 0: the vendor chain (Glink, Smp2p, ClockDxe, SPMI) does not
  survive this board, which is why the USB state is read out rather than inherited.
- GPU: the Adreno 732 values are recorded and audited; the on-device GMU/CP/SMMU path is untested.
- The Adreno archives, the Metal plug-in blob and the A732 firmware pins need inputs a public runner
  cannot produce, so they are not in the CI archive list.

Documentation: `docs/STATE.md` (state and next steps), `docs/PROGRESS.md` (history), `KERNEL-BUILD.md`
(CI notes), `docs/RECOVERY-LOG.md` (reading the log on the tablet), `docs/GPU-PORT.md`,
`docs/TOUCH-PORT.md`, `docs/DPU-TASKS.md`.
