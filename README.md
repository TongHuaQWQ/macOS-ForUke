# macOS-ForUke

Device repository for the Xiaomi Pad 7 / POCO Pad X1 (`uke`, SM7675 "Cliffs", Adreno 732), running
**macOS 26.5.2 (build 25F84)** through UEFI with a self-built XNU.

## 1. What this is

This repository is the board description of one tablet for the UniXNU build driver, plus the
board-side code that description needs: `board.toml` (component selection, IPSW identity, userdata
geometry, payload and ADT values), `configs/*_defconfig`, the kernel patch series under
`patches/xnu/`, the kernel-side packages under `package/` (materialised in the build tree as
`kits/uke`), the board copies of the pinned components under `drivers/` and `kits/`, and the GitHub
Actions workflows that build the kernel on a macOS runner.

It is the **device** layer of the boot chain — build-time only. What it produces is the resolved
kernel configuration and archive list, and through CI the prelinked `kernel.macho`; from that kernel
the local round builds the payload FAT volume and `vendor_boot_a`. It contributes nothing at runtime:
the firmware image (`boot_a`) comes from the UEFI tree, and the kernel it packs is this repository's
configuration of Apple's own kernelcache kexts.

Format 2: `board.toml` declares `schema_version = 2`, the SM7675 SoC layer is pinned in
`feeds.toml`, the OS identity lives in the shared profile `macos-26.5.2-25F84`, and `porting.toml`
records the state of each hardware area. `AGENTS.md` has the editing rules; `docs/STATE.md` is the
handoff record and `docs/PROGRESS.md` the history behind it.

## 2. Where it sits

Three layers, three repositories, this one on top:

| Layer | Repository | Produces |
| --- | --- | --- |
| core | `TongHuaQWQ/UniXNU`, branch `macos` (fork of `UniXNU/UniXNU`) | `./unixnu`: the kernel build, the prelink, the payload, the boot image |
| device | this one | `board.toml`, `configs/`, patch series, kernel-side packages, CI workflows |
| firmware | `uefi-uke` (Mu-Silicium fork, `Platforms/Xiaomi/ukePkg`) | `boot.img` — UEFI, UniXNU menu, DarwinBoot |

Nothing pins this repository; it is the top of the graph. What it pins, and where:

| Source | Pin | Where |
| --- | --- | --- |
| core `TongHuaQWQ/UniXNU@macos` | workflow default `UKE_CORE_REPO` / `UKE_CORE_REF` | `.github/workflows/build-kernel-min.yml` |
| `sm7675` SoC layer, `https://github.com/Allenwdk/UniXNU-sm7675-common` | `commit = "98618c197e789acfd7ac6755ad23bf7e04881780"`, plus `local = "../UniXNU-sm7675-common"` | `feeds.toml` `[[source]] name = "sm7675"` |
| `ArmCpu`, `https://github.com/TongHuaQWQ/ArmCpu.git` | `commit = "6288238"`, plus board copy `local = "drivers/ArmCpu"` | `feeds.toml` `[[source]] name = "ArmCpu"` |
| `XnuPrelink` | board copy `local = "kits/XnuPrelink"` (no git pin — first-wins resolution) | `feeds.toml` `[[source]] name = "XnuPrelink"`, consumed by `board.toml` `[kernel.prelink] tool = "kits/XnuPrelink/emit_prelink.py"` |
| `DarwinBoot` | no pin here: it arrives as the core's `drivers/DarwinBoot` submodule | `board.toml` `[firmware] packages = [… "{tree}/drivers/DarwinBoot" …]` |
| `ShengUfs`, `ShengNovatekTouch` | board copies only | `feeds.toml` `local = "drivers/NativeUfs"` and `local = "drivers/NativeNovatekTouch"` |
| `Adreno740`, `GopFramebuffer`, `AdrenoMetal` | `Allenwdk/NativeAdreno@c950906…`, `Allenwdk/NativeMobileDisplay@cd8a7b2…`, `Allenwdk/AdrenoMetal@739de5b…` | `feeds.toml` |

Boot chain on the tablet: `fastboot` → ABL → UEFI (`boot_a`) → `UniXNU.efi` → DarwinBoot (in the
firmware volume) → XNU (`vendor_boot_a` payload). See `docs/STATE.md` §1 for the diagram the port
actually follows.

## 3. Dependencies

| Input | Comes from | Without it |
| --- | --- | --- |
| macOS host with Xcode (`xcodebuild`, `iphoneos` SDK, MIG) | App Store / developer.apple.com; CI uses `runs-on: macos-latest` | The kernel step cannot run at all: XNU's makefiles are driven by `xcodebuild` and MIG. The device side (payload, boot image) is Linux/macOS-agnostic. |
| `git`, `make`, `clang`, `cmake`, `ninja`, `unifdef`, `mtools`, `dtc`, `python3` | `brew install cmake ninja unifdef llvm mtools dtc`; the workflow verifies each one | `./unixnu` refuses or the archive recipes fail. `build-kernel-min.yml` prints the missing tool names and stops. |
| core `TongHuaQWQ/UniXNU@macos` | GitHub, checked out beside this repo as `UniXNU/` | No `./unixnu`; nothing resolves. |
| `UniXNU-sm7675-common` (SoC layer) | `https://github.com/Allenwdk/UniXNU-sm7675-common` at `98618c1…`; also expected as a sibling `../UniXNU-sm7675-common` | `soc sm7675: … is not a directory` on a clean runner — which is why the workflow clones it explicitly. |
| macOS 26.5.2 (25F84) universal IPSW | `[ipsw] url` (Apple CDN) in `board.toml`; `--ipsw`/`UNIXNU_IPSW` overrides | No kernelcache to pair against: the prelink has nothing to emit a collection from (`unixnu ipsw fetch`). |
| Apple's `kernelcache.release.mac17g` inside that IPSW | `[ipsw]` identity fields select it; pinned by `[kernel.prelink] kernelcache_sha256` | The pin fails at fetch time; a wrong cache silently produces a wrong prelink. |
| `uefi-uke` UEFI tree (Mu-Silicium fork) | **Not published.** This repository carries the firmware-layer changes as patches in `firmware/` (`uke-firmware.patch`, `uke-firmware-worktree-wip.patch`, `files/`) to be applied to `Project-Silicium/Mu-Silicium` | No firmware volume, so no payload volume, no `boot.img`/`vendor_boot` pair. The payload itself only needs the firmware for its FV and the DarwinBoot INF. |
| Platform seed (`--unique-chip-id`) | Read from the device; `board.toml` `[payload.adt] unique_chip_id = "required"`. **Not stored in any repository** | The payload cannot be built; the ADT keeps a placeholder. |
| Trust cache | `[payload] trust_cache_sha256` accepts one hash; the bundle itself is Apple data from the IPSW/device | Payload refuses the cache it is given; the kernel will not load the collection kexts. |
| A732 GPU firmware (`.cache/firmware/uke-gpu`) | Extracted from this unit's `/vendor/firmware_mnt/image` and `/odm/firmware`, see `proprietary-files.txt`; pinned by `[gpu.firmware_pins]` | GPU archives cannot be built; they are therefore absent from the CI archive list and `Adreno740`/`AdrenoMetal` are deselected in `board.toml`. |
| Owner-built Metal plug-in bundle | Produced by `build-gpu-addon-blob.py` from a tree that already has it | `payloads/kernel.macho.gpuaddon` is missing; `sheng-gpu-addon=1` has nothing to stage. |
| `work/round.py`, `work/flash/`, `work/analysis/log-capture/` | **Not in this repository.** They live in the port's workspace harness, outside the checkout (`python3 work/round.py <run-id> --name bNN-<tag>` runs from that harness root) | The one-command round and the TWRP log reader below are unavailable from a clean clone; build the payload with `./unixnu build payload`/`bootimg` instead. |

## 4. Build and use

The kernel is ~30 minutes of CI: XNU is compiled and prelinked on a macOS runner while the device
side is Linux. CI (`.github/workflows/build-kernel-min.yml`):

```sh
# push a ci-* branch to the device remote: the workflow runs with the round's defaults
#   UKE_DEFCONFIG=uke-macos_defconfig  UKE_CORE_REPO=TongHuaQWQ/UniXNU  UKE_CORE_REF=macos
git push <remote> ci-<tag>
```

`workflow_dispatch` needs a token scope this setup does not have, so a branch push is the trigger that
works. The workflow runs, in order: `python3 scripts/check-patch-copies.py`, the component self-check,
the host-tool install, the SoC-layer clone, then

```sh
./unixnu fetch xnu libplatform libdispatch AvailabilityVersions libpthread \
                corecrypto-2023 IOStorageFamily -D ../UniXNU-uke
./unixnu ipsw fetch -D ../UniXNU-uke
./unixnu defconfig bootstrap_defconfig -D ../UniXNU-uke
./unixnu build kernel --skip-prelink -D ../UniXNU-uke      # pass 1: compile records + candidate
./unixnu defconfig "$UKE_DEFCONFIG" -D ../UniXNU-uke
./unixnu tree -D ../UniXNU-uke
./unixnu build kernel -D ../UniXNU-uke                     # link, archives, prelink
./unixnu status --json -D ../UniXNU-uke                    # resolves out/<device>/<os_profile>/<config>/
```

The artifact is `uke-kernel-min`: `kernel.macho`, `kernel.json`, `kernel.map.json`, the
symbol-bearing native kernel under `native/`, and the build logs.

Host-only validation (`.github/workflows/ci.yml`, Ubuntu, ~15 min):

```sh
python3 -c "import pathlib,tomllib,yaml; [yaml.safe_load(p.read_text()) for p in pathlib.Path('.github/workflows').glob('*.yml')]; tomllib.loads(pathlib.Path('board.toml').read_text()); tomllib.loads(pathlib.Path('feeds.toml').read_text()); tomllib.loads(pathlib.Path('unixnu.lock').read_text()); print('ok workflow/board/feeds/lock')"
python3 scripts/check-uke-gpu.py
python3 scripts/check-uke-gpu-host.py --kit ../UniXNU/drivers/NativeAdreno
python3 scripts/check-gpu-power-host.py
bash scripts/check-xnu-patch-set.sh
```

The porting record is checked with the shared checker, from a UniXNU checkout
(`.github/workflows/porting-record.yml`):

```sh
python3 unixnu/scripts/porting-record.py check --device device
```

Flash and read back (the ABL answers `Failed to load/authenticate boot image: Load Error` to
`fastboot boot`, so always flash):

```sh
# 1. flash the round (boot_a is the firmware, vendor_boot_a is the kernel + payload)
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

# 4. dump and decode the window (workspace script, not shipped here)
bash ../analysis/log-capture/read-ukelog-twrp.sh            # geometry, dd, decode
bash ../analysis/log-capture/read-ukelog-twrp.sh --self-test # prove the window itself first
```

Use `fastboot set_active a` before each test: the ABL marks a slot unbootable after repeated
non-Android boots.

`configs/uke-macos_defconfig` is the round's configuration: source corecrypto, pthread, mockfs,
`ArmDTPlatform`, `ArmCpu`, the USB diagnostic console, the log writer (`CONFIG_XNU_BOOT_LOG` +
`CONFIG_XNU_LOG_USERDATA`, force-loading `libUkeUfsLog.a` for the userdata half), and the fileset
reference kernel the prelink compares against. `CONFIG_XNU_NATIVE_UFS`, `CONFIG_XNU_NATIVE_AES` and
`CONFIG_XNU_SHENG_PMGR` are off here; the header of that file says why, and it is not an oversight.
The other configurations are kept for the paths they cover: `uke_defconfig` (first console),
`uke-storage_defconfig`, `uke-userland_defconfig`, `uke-full_defconfig`,
`uke-full-plus_defconfig` (sheng parity, needs inputs CI cannot produce), `uke-gpu_defconfig`,
`uke-nodisplay_defconfig`, `uke-cpu_defconfig`, `bootstrap_defconfig`.

## 5. See also

Official guides (the specification this repository is written against):

- [Port a device — the eight steps](https://unixnu.com/zh/porting/steps), in particular step 3
  (create the device repository) and step 8 (finish and publish);
- [Device repository](https://unixnu.com/zh/porting/device-repository) — the required file list,
  completion checklist, `feeds.toml`, `board.toml` and record rules;
- [Drivers](https://unixnu.com/zh/porting/drivers) — the order in which hardware areas are adapted;
- [Device packaging script](https://unixnu.com/zh/porting/packaging), [log capture](https://unixnu.com/zh/porting/log-capture),
  [bring-up verification order](https://unixnu.com/zh/porting/bring-up), [migration](https://unixnu.com/zh/porting/migration);
- [Build environment](https://unixnu.com/zh/guide/requirements) and [build and boot](https://unixnu.com/zh/building).

This repository's own documents:

- `AGENTS.md` — editing rules; `KERNEL-BUILD.md` — CI notes;
- `docs/STATE.md` — the handoff record (current state, next steps); `docs/PROGRESS.md` — history;
- `docs/MACOS-BUILD.md`, `docs/RECOVERY-LOG.md` (reading the log on the tablet), `docs/GPU-PORT.md`,
  `docs/TOUCH-PORT.md`, `docs/DPU-PORT.md`, `docs/DPU-TASKS.md`, `docs/ARM64E-PORT.md`,
  `docs/GPU-SHENG-COMPARE.md`;
- device records: `porting.toml` (per-area status, inputs, evidence) and `board.toml`.

Known gap against the specification: `scripts/package.sh` and `.github/workflows/package.yml` are
required of every device repository but are **not present here**; the round is driven by
`build-kernel-min.yml` plus the workspace scripts instead.

## Proven on hardware

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
board), and the tail of the log window overlapping foreign filesystem metadata (518 bytes at the
window's last 16 KiB slot), so the window has to shrink before it can be written for a full hour.

## Diagnostics

| Channel | Read with | Survives |
| --- | --- | --- |
| panel QR card | `work/analysis/log-capture/panel_qr_decode.py` (photo or 4K recording; a 3200×2136 panel must not be downscaled below ~3800 px) | needs a camera |
| userdata window | `work/analysis/log-capture/read-ukelog-twrp.sh` → `tools/decode-ukelog.py` | reset, power cycle |
| pram (msgbuf snapshot, panic text, writer beacon) | the firmware menu on the next boot (`UkeBootMemoryDxe` scans for `panic(` and prints pram+0x1100) | warm reset only |
| msgbuf over USB | `package/qcom-usb-debug/scripts/collect-xnu-usb.py` | needs enumeration, which is the current blocker |

The writer reports its own progress into the pram beacon slot, so a boot whose window is empty still
names the stage: `UKELOG-TRANSPORT-OK`, `UKELOG-NOTRANSPORT`, `UKELOG-WINDOW-OPEN`, `UKELOG-NOWINDOW`.

The kernel-side packages, linked as `kits/uke`: `package/UkeLastLog.c` (msgbuf → pram and msgbuf →
userdata log writer); `package/UfsLog.cpp` + `package/UfsLogKernel.c` (the freestanding UFS transport
behind that writer, without IOKit — `scripts/check-ufs-log.py` compiles and links both for
`aarch64-none-elf` on the host in about a second); `package/qcom-usb-debug` (the DWC3 device-mode
console and panel diagnostics); `package/CrashLogProbe.c`, `package/BootWatchdogPolicy.c`,
`package/MissingKernelSymbols.c`.

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
- `scripts/package.sh` / `.github/workflows/package.yml` (required by the device-repository
  specification) do not exist yet; the round is driven by `build-kernel-min.yml`.
